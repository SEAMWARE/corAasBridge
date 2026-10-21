//
// FILE            aasBridge.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// The AAS server's shells and submodels as AssetChannels, written back, and its Operations - see header.
//
#include <pthread.h>                                  // pthread_*
#include <signal.h>                                   // sigset_t, SIGPIPE
#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // int64_t
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // malloc, calloc, free
#include <string.h>                                   // strcmp, strdup, strlen, strncmp
#include <time.h>                                     // clock_gettime, gmtime_r
#include <unistd.h>                                   // usleep

#include "corLog/corLog.h"                            // COR_E, COR_W, COR_I, COR_V
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAllocBufferInit.h"              // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"             // corAllocBufferReset
#include "corAlloc/corAllocStrdup.h"                  // corAllocStrdup
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeObject, corTreeArray, corTreeString, corTreeChildAdd
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corBridge/BridgeBroker.h"                   // BridgeBroker, BRIDGE_*
#include "corBridge/BridgeDriver.h"                   // BridgeDirection

#include "aasHttp.h"                                  // AasTarget, aasRequest, aasHttpError
#include "aasModel.h"                                 // aasModelRead, AasRead, AasLeaf, aasValue*
#include "aasBridge.h"                                // Own interface



//
// QUEUE_MAX    - the writes (and the invocations) waiting
// TIMEOUT_MS   - a request's: a read, a write, an invocation's answer
// INVOKE_TIME  - how long the server may take over an Operation (its clientTimeoutDuration)
//
#define QUEUE_MAX     256
#define TIMEOUT_MS    10000
#define INVOKE_TIME   "PT60S"
#define INVOKE_MS     65000



// -----------------------------------------------------------------------------
//
// Endpoint - an AssetChannel's model root, and what its last read found
//
typedef struct Endpoint
{
  char*            root;
  BridgeDirection  direction;
  int64_t          pollMs;
  int64_t          nextPoll;                          // monotonic ms; 0: at once
  char*            skeleton;                          // the last model handed over; NULL: none yet
  AasLeaf*         leafV;                             // its leaves, their last values
  int              leaves;
  unsigned int     generation;                        // a write queued or done: a read across it compares no values
  int              lastStatus;                        // -1: never read
  char             lastWhy[512];
  bool             available;
} Endpoint;



// -----------------------------------------------------------------------------
//
// Job - a write of a leaf (the writer's), or an Operation invoked (the invoker's)
//
typedef struct Job
{
  char*         leafId;                               // a write's
  char*         path;
  char*         body;
  char*         echo;                                 // the value as the next read renders it
  AasTarget     target;                               // an invocation's - its own TLS context
  char*         executionId;
  char*         input;
  struct Job*   next;
} Job;



// -----------------------------------------------------------------------------
//
// Queue -
//
typedef struct Queue
{
  Job*            head;
  Job*            tail;
  int             count;
  pthread_cond_t  cond;
} Queue;



static AasTarget           target;
static char*               authorization = NULL;     // for an Operation on target's server
static char*               caFile        = NULL;
static bool                insecure      = false;
static const BridgeBroker* brokerP       = NULL;
static volatile bool       stop          = false;
static volatile bool       answered      = false;   // the server has answered, once: a write can go out
static bool                threads       = false;
static pthread_t           pollThread;
static pthread_t           writerThread;
static pthread_t           invokerThread;

static pthread_mutex_t     mutex         = PTHREAD_MUTEX_INITIALIZER;
static Endpoint**          endpointV     = NULL;
static int                 endpoints     = 0;
static Queue               writes        = { NULL, NULL, 0, PTHREAD_COND_INITIALIZER };
static Queue               invocations   = { NULL, NULL, 0, PTHREAD_COND_INITIALIZER };



// -----------------------------------------------------------------------------
//
// nowMs - monotonic
//
static int64_t nowMs(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}



// -----------------------------------------------------------------------------
//
// nowIso - the wall clock as ISO 8601, UTC, milliseconds: a value's sourceTime
//
static void nowIso(char* buf, int size, int64_t* nsP)
{
  struct timespec ts;
  struct tm       tm;

  clock_gettime(CLOCK_REALTIME, &ts);
  gmtime_r(&ts.tv_sec, &tm);
  snprintf(buf, size, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
           tm.tm_hour, tm.tm_min, tm.tm_sec, (int) (ts.tv_nsec / 1000000));
  *nsP = (int64_t) ts.tv_sec * 1000000000 + ts.tv_nsec;
}



// -----------------------------------------------------------------------------
//
// sigpipeBlock - a write to a server that has closed the connection fails; it must not kill the broker
//
static void sigpipeBlock(void)
{
  sigset_t set;

  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &set, NULL);
}



// -----------------------------------------------------------------------------
//
// statusIn - whether a channel carries, and why not (ABI 12)
//
static void statusIn(const char* root, bool available, const char* why)
{
#if BRIDGE_ABI_VERSION >= 12
  if ((brokerP != NULL) && (brokerP->abiVersion >= 12) && (brokerP->channelStatusIn != NULL))
    brokerP->channelStatusIn("aas", root, (available == true) ? BridgeChannelAvailable : BridgeChannelDormant, why);
#else
  (void) root;
  (void) available;
  (void) why;
#endif
}



// -----------------------------------------------------------------------------
//
// endpointFind - under the mutex
//
static Endpoint* endpointFind(const char* root)
{
  for (int ix = 0; ix < endpoints; ix++)
  {
    if (strcmp(endpointV[ix]->root, root) == 0)
      return endpointV[ix];
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// leafFind - the leaf of that id in the last model of an endpoint; under the mutex
//
static AasLeaf* leafFind(const char* id, Endpoint** epPP)
{
  for (int ix = 0; ix < endpoints; ix++)
  {
    Endpoint* epP = endpointV[ix];

    for (int lx = 0; lx < epP->leaves; lx++)
    {
      if (strcmp(epP->leafV[lx].id, id) == 0)
      {
        if (epPP != NULL)
          *epPP = epP;
        return &epP->leafV[lx];
      }
    }
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// leavesFree -
//
static void leavesFree(AasLeaf* leafV, int leaves)
{
  for (int ix = 0; ix < leaves; ix++)
    aasLeafFree(&leafV[ix]);
  free(leafV);
}



// -----------------------------------------------------------------------------
//
// endpointFree -
//
static void endpointFree(Endpoint* epP)
{
  free(epP->root);
  free(epP->skeleton);
  leavesFree(epP->leafV, epP->leaves);
  free(epP);
}



// -----------------------------------------------------------------------------
//
// pollOne - one read of an endpoint's model: handed over as the model, or as the values that changed
//
static void pollOne(const char* root, unsigned int generation)
{
  CorAlloc ka;
  char     kaBuf[32 * 1024];
  AasRead  read;
  char     why[512];

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 256 * 1024, NULL, "aas read");

  int status = aasModelRead(&target, &ka, root, &read, why, sizeof(why));

  pthread_mutex_lock(&mutex);

  Endpoint* epP = endpointFind(root);

  if (epP == NULL)                                    // deleted while it was read
  {
    pthread_mutex_unlock(&mutex);
    aasReadRelease(&read);
    corAllocBufferReset(&ka, false);
    return;
  }

  epP->nextPoll = nowMs() + epP->pollMs;

  if (status != 200)
  {
    bool    news   = (epP->lastStatus != status) || (strcmp(epP->lastWhy, why) != 0);
    int64_t pollMs = epP->pollMs;

    epP->lastStatus = status;
    epP->available  = false;
    snprintf(epP->lastWhy, sizeof(epP->lastWhy), "%s", why);
    pthread_mutex_unlock(&mutex);

    if (news == true)
    {
      COR_W("aas: '%s': %s - read again every %lld ms", root, why, (long long) pollMs);
      statusIn(root, false, why);
    }

    corAllocBufferReset(&ka, false);
    return;
  }

  answered = true;

  bool    wasAvailable = epP->available;
  char*   sample       = NULL;
  char    time[64];
  int64_t timeNs;

  epP->available  = true;
  epP->lastStatus = 200;
  epP->lastWhy[0] = 0;

  nowIso(time, sizeof(time), &timeNs);

  if ((epP->skeleton == NULL) || (strcmp(epP->skeleton, read.skeleton) != 0))
  {
    //
    // The first read, or the structure changed: the model, whole - and its leaves are the ones kept
    //
    sample = aasRender(read.sampleP);

    free(epP->skeleton);
    leavesFree(epP->leafV, epP->leaves);

    epP->skeleton = read.skeleton;
    epP->leafV    = read.leafV;
    epP->leaves   = read.leaves;
    read.skeleton = NULL;
    read.leafV    = NULL;
    read.leaves   = 0;

    for (int ix = 0; ix < epP->leaves; ix++)
      epP->leafV[ix].valueP = NULL;

    COR_V("aas: '%s': the model (%d leaves)", root, epP->leaves);
  }
  else if (epP->generation == generation)
  {
    //
    // The same structure - the same leaves, in the same order: those whose value changed
    //
    CorNode* sampleP = corTreeObject(&ka, NULL);
    CorNode* valuesP = corTreeArray(&ka, "values");
    char*    timeKa  = corAllocStrdup(&ka, time);

    corTreeChildAdd(sampleP, valuesP);

    for (int ix = 0; ix < epP->leaves; ix++)
    {
      AasLeaf* oldP = &epP->leafV[ix];
      AasLeaf* newP = &read.leafV[ix];

      if ((oldP->valueJson != NULL) && (strcmp(oldP->valueJson, newP->valueJson) == 0))
        continue;

      CorNode* itemP = corTreeObject(&ka, NULL);

      corTreeChildAdd(itemP, corTreeString(&ka, "id", newP->id));
      corTreeChildAdd(itemP, corTreeClone(&ka, newP->valueP));
      corTreeChildAdd(itemP, corTreeString(&ka, "sourceTime", timeKa));
      corTreeChildAdd(valuesP, itemP);

      free(oldP->valueJson);
      oldP->valueJson = newP->valueJson;
      newP->valueJson = NULL;
    }

    if (valuesP->value.head != NULL)
      sample = aasRender(sampleP);
  }

  pthread_mutex_unlock(&mutex);

  if (wasAvailable == false)
  {
    COR_I("aas: '%s' read from %s:%d", root, target.host, target.port);
    statusIn(root, true, NULL);
  }

  if ((sample != NULL) && (brokerP != NULL))
  {
    int r = brokerP->sampleIn("aas", root, sample, timeNs);

    COR_V("aas: sample on '%s' (%d): %.300s", root, r, sample);
    if ((r != BRIDGE_OK) && (r != BRIDGE_NOT_FOUND))
      COR_W("aas: the sample on '%s' was not taken (%d)", root, r);
  }

  free(sample);
  aasReadRelease(&read);
  corAllocBufferReset(&ka, false);
}



// -----------------------------------------------------------------------------
//
// pollMain - each endpoint read when it is due
//
static void* pollMain(void* arg)
{
  (void) arg;
  sigpipeBlock();

  while (stop == false)
  {
    char*        root       = NULL;
    unsigned int generation = 0;
    int64_t      now        = nowMs();

    pthread_mutex_lock(&mutex);

    Endpoint* dueP = NULL;

    for (int ix = 0; ix < endpoints; ix++)
    {
      if ((endpointV[ix]->nextPoll <= now) && ((dueP == NULL) || (endpointV[ix]->nextPoll < dueP->nextPoll)))
        dueP = endpointV[ix];
    }

    if (dueP != NULL)
    {
      root       = strdup(dueP->root);
      generation = dueP->generation;
    }

    pthread_mutex_unlock(&mutex);

    if (root == NULL)
    {
      usleep(50 * 1000);
      continue;
    }

    pollOne(root, generation);
    free(root);
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// jobFree -
//
static void jobFree(Job* jP)
{
  free(jP->leafId);
  free(jP->path);
  free(jP->body);
  free(jP->echo);
  free(jP->executionId);
  free(jP->input);
  aasTargetRelease(&jP->target);
  free(jP);
}



// -----------------------------------------------------------------------------
//
// jobAdd - to a queue; false: it is full. Under the mutex
//
static bool jobAdd(Queue* qP, Job* jP)
{
  if (qP->count >= QUEUE_MAX)
    return false;

  if (qP->tail == NULL)
    qP->head = jP;
  else
    qP->tail->next = jP;

  qP->tail = jP;
  qP->count++;

  pthread_cond_signal(&qP->cond);
  return true;
}



// -----------------------------------------------------------------------------
//
// jobNext - the next job of a queue, waiting for one; NULL: stopped
//
static Job* jobNext(Queue* qP)
{
  pthread_mutex_lock(&mutex);

  while ((qP->head == NULL) && (stop == false))
    pthread_cond_wait(&qP->cond, &mutex);

  Job* jP = NULL;

  if (stop == false)
  {
    jP       = qP->head;
    qP->head = jP->next;
    if (qP->head == NULL)
      qP->tail = NULL;
    qP->count--;
  }

  pthread_mutex_unlock(&mutex);
  return jP;
}



// -----------------------------------------------------------------------------
//
// leafWrite - PATCH a leaf's $value; refused, the leaf is read back
//
static void leafWrite(Job* jP)
{
  char* answer = NULL;
  int   status = aasRequest(&target, "PATCH", jP->path, jP->body, &answer, TIMEOUT_MS);
  bool  ok     = (status >= 200) && (status <= 299);

  if (ok == true)
    COR_V("aas: PATCH %s %s: %d", jP->path, jP->body, status);
  else if (status == 0)
    COR_W("aas: PATCH %s %s: %s - the element is read back", jP->path, jP->body, aasHttpError());
  else
    COR_W("aas: PATCH %s %s: %d %.300s - the element is read back", jP->path, jP->body, status, (answer != NULL) ? answer : "");

  free(answer);

  pthread_mutex_lock(&mutex);

  Endpoint* epP   = NULL;
  AasLeaf*  leafP = leafFind(jP->leafId, &epP);

  if (leafP != NULL)
  {
    free(leafP->valueJson);

    if (ok == true)
    {
      leafP->valueJson = jP->echo;                    // the server has it: its next read is no change
      jP->echo         = NULL;
    }
    else
    {
      leafP->valueJson = NULL;                        // unknown: the next read hands it over, whatever it is
      epP->nextPoll    = 0;
    }

    epP->generation++;
  }

  pthread_mutex_unlock(&mutex);
}



// -----------------------------------------------------------------------------
//
// writerMain - the writes, one at a time
//
static void* writerMain(void* arg)
{
  (void) arg;
  sigpipeBlock();

  Job* jP;

  while ((jP = jobNext(&writes)) != NULL)
  {
    leafWrite(jP);
    jobFree(jP);
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// serviceEnd - an invocation's outcome to the broker: output (JSON) or why it failed
//
static void serviceEnd(const char* executionId, const char* output, const char* why)
{
#if BRIDGE_ABI_VERSION >= 11
  if ((brokerP == NULL) || (brokerP->abiVersion < 11) || (brokerP->serviceUpdateIn == NULL))
    return;

  int r;

  if (why == NULL)
    r = brokerP->serviceUpdateIn("aas", executionId, "completed", NULL, output, NULL);
  else
  {
    CorAlloc ka;
    char     kaBuf[4096];

    corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 4096, NULL, "aas problem");

    CorNode* pdP = corTreeObject(&ka, NULL);

    corTreeChildAdd(pdP, corTreeString(&ka, "type",   "https://uri.etsi.org/ngsi-ld/errors/InternalError"));
    corTreeChildAdd(pdP, corTreeString(&ka, "title",  "The AAS operation failed"));
    corTreeChildAdd(pdP, corTreeInteger(&ka, "status", 502));
    corTreeChildAdd(pdP, corTreeString(&ka, "detail", (char*) why));

    char* pd = aasRender(pdP);

    r = brokerP->serviceUpdateIn("aas", executionId, "failed", NULL, NULL, pd);
    free(pd);
    corAllocBufferReset(&ka, false);
  }

  if ((r != BRIDGE_OK) && (r != BRIDGE_NOT_FOUND))
    COR_W("aas: the broker did not take the end of execution '%s' (%d)", executionId, r);
#else
  (void) executionId;
  (void) output;
  (void) why;
#endif
}



// -----------------------------------------------------------------------------
//
// argumentsAdd - an Operation's variables (inputVariables or inoutputVariables) as its arguments, from the
// input by idShort - each one converted by its valueType. NULL: done; else why not
//
static const char* argumentsAdd(CorAlloc* kaP, CorNode* argsP, CorNode* varsP, CorNode* inputP, char* whyBuf, int whyLen)
{
  for (CorNode* varP = (varsP != NULL) ? varsP->value.head : NULL; varP != NULL; varP = varP->next)
  {
    CorNode*    valueP    = (varP->type == CorObject) ? corTreeLookup(varP, "value") : NULL;
    CorNode*    idShortP  = ((valueP != NULL) && (valueP->type == CorObject)) ? corTreeLookup(valueP, "idShort") : NULL;
    CorNode*    typeP     = (idShortP != NULL) ? corTreeLookup(valueP, "modelType") : NULL;
    CorNode*    vtP       = (idShortP != NULL) ? corTreeLookup(valueP, "valueType") : NULL;
    const char* valueType = ((vtP != NULL) && (vtP->type == CorString)) ? vtP->value.s : NULL;

    if ((idShortP == NULL) || (idShortP->type != CorString))
      continue;

    const char* idShort = idShortP->value.s;

    if ((typeP == NULL) || (typeP->type != CorString) || (strcmp(typeP->value.s, "Property") != 0))
    {
      snprintf(whyBuf, whyLen, "the operation's variable '%.100s' is no Property - only Properties are taken", idShort);
      return whyBuf;
    }

    CorNode* inP = corTreeLookup(inputP, idShort);

    if (inP == NULL)
    {
      snprintf(whyBuf, whyLen, "the input has no '%.100s' - an input variable of the operation", idShort);
      return whyBuf;
    }

    char why[256];

    if (aasValueCheck(AasLeafProperty, valueType, inP, why, sizeof(why)) != NULL)
    {
      snprintf(whyBuf, whyLen, "the input variable '%.100s': %s", idShort, why);
      return whyBuf;
    }

    char* text = aasValueText(inP);

    if (text == NULL)
    {
      snprintf(whyBuf, whyLen, "out of memory");
      return whyBuf;
    }

    CorNode* argP  = corTreeObject(kaP, NULL);
    CorNode* propP = corTreeObject(kaP, "value");

    corTreeChildAdd(propP, corTreeString(kaP, "modelType", "Property"));
    corTreeChildAdd(propP, corTreeString(kaP, "idShort",   (char*) idShort));
    if (valueType != NULL)
      corTreeChildAdd(propP, corTreeString(kaP, "valueType", (char*) valueType));
    corTreeChildAdd(propP, corTreeString(kaP, "value", corAllocStrdup(kaP, text)));
    corTreeChildAdd(argP, propP);
    corTreeChildAdd(argsP, argP);
    free(text);
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// isVariable - an input member names one of the variables
//
static bool isVariable(CorNode* varsP, const char* name)
{
  for (CorNode* varP = (varsP != NULL) ? varsP->value.head : NULL; varP != NULL; varP = varP->next)
  {
    CorNode* valueP   = (varP->type == CorObject) ? corTreeLookup(varP, "value") : NULL;
    CorNode* idShortP = ((valueP != NULL) && (valueP->type == CorObject)) ? corTreeLookup(valueP, "idShort") : NULL;

    if ((idShortP != NULL) && (idShortP->type == CorString) && (strcmp(idShortP->value.s, name) == 0))
      return true;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// outputAdd - an OperationResult's arguments, by idShort, converted by their valueType
//
static void outputAdd(CorAlloc* kaP, CorNode* outputP, CorNode* argsP)
{
  for (CorNode* argP = ((argsP != NULL) && (argsP->type == CorArray)) ? argsP->value.head : NULL; argP != NULL; argP = argP->next)
  {
    CorNode* valueP   = (argP->type == CorObject) ? corTreeLookup(argP, "value") : NULL;
    CorNode* idShortP = ((valueP != NULL) && (valueP->type == CorObject)) ? corTreeLookup(valueP, "idShort") : NULL;
    CorNode* vP       = (idShortP != NULL) ? corTreeLookup(valueP, "value") : NULL;
    CorNode* vtP      = (idShortP != NULL) ? corTreeLookup(valueP, "valueType") : NULL;

    if ((idShortP == NULL) || (idShortP->type != CorString) || (corTreeLookup(outputP, idShortP->value.s) != NULL))
      continue;

    if ((vP != NULL) && (vP->type == CorString))
      corTreeChildAdd(outputP, aasTypedValue(kaP, idShortP->value.s, ((vtP != NULL) && (vtP->type == CorString)) ? vtP->value.s : NULL, vP->value.s));
    else if (vP != NULL)
    {
      CorNode* copyP = corTreeClone(kaP, vP);

      copyP->name = idShortP->value.s;
      corTreeChildAdd(outputP, copyP);
    }
  }
}



// -----------------------------------------------------------------------------
//
// invoke - an Operation: its variables read, the input made its arguments, POST .../invoke
//
static void invoke(Job* jP)
{
  CorAlloc ka;
  char     kaBuf[16 * 1024];
  char     why[1024];
  char*    body   = NULL;
  char*    output = NULL;

  why[0] = 0;
  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 64 * 1024, NULL, "aas invoke");

  int      status = aasRequest(&jP->target, "GET", "", NULL, &body, TIMEOUT_MS);
  CorNode* opP    = ((status == 200) && (body != NULL)) ? aasParse(&ka, body) : NULL;
  CorNode* typeP  = ((opP != NULL) && (opP->type == CorObject)) ? corTreeLookup(opP, "modelType") : NULL;

  free(body);
  body = NULL;

  if (status == 0)
    snprintf(why, sizeof(why), "the operation could not be read: %s", aasHttpError());
  else if (status == 404)
    snprintf(why, sizeof(why), "the server has no such operation");
  else if (status != 200)
    snprintf(why, sizeof(why), "the operation could not be read: %d", status);
  else if ((typeP == NULL) || (typeP->type != CorString) || (strcmp(typeP->value.s, "Operation") != 0))
    snprintf(why, sizeof(why), "the element is no Operation");

  CorNode* inputP = (why[0] == 0) ? aasParse(&ka, jP->input) : NULL;

  if ((why[0] == 0) && ((inputP == NULL) || (inputP->type != CorObject)))
    snprintf(why, sizeof(why), "the input is no JSON object");

  CorNode* inVarsP    = (why[0] == 0) ? corTreeLookup(opP, "inputVariables")    : NULL;
  CorNode* inoutVarsP = (why[0] == 0) ? corTreeLookup(opP, "inoutputVariables") : NULL;

  //
  // Each member of the input a variable - but the entity's own id and type, which the broker puts in it
  //
  for (CorNode* inP = (why[0] == 0) ? inputP->value.head : NULL; inP != NULL; inP = inP->next)
  {
    if ((strcmp(inP->name, "id") == 0) || (strcmp(inP->name, "type") == 0))
      continue;

    if ((isVariable(inVarsP, inP->name) == false) && (isVariable(inoutVarsP, inP->name) == false))
    {
      snprintf(why, sizeof(why), "'%.100s' is no input variable of the operation", inP->name);
      break;
    }
  }

  CorNode* requestP = corTreeObject(&ka, NULL);
  CorNode* inArgsP  = corTreeArray(&ka, "inputArguments");
  CorNode* ioArgsP  = corTreeArray(&ka, "inoutputArguments");

  corTreeChildAdd(requestP, inArgsP);
  corTreeChildAdd(requestP, ioArgsP);
  corTreeChildAdd(requestP, corTreeString(&ka, "clientTimeoutDuration", INVOKE_TIME));

  if (why[0] == 0)
    argumentsAdd(&ka, inArgsP, inVarsP, inputP, why, sizeof(why));
  if (why[0] == 0)
    argumentsAdd(&ka, ioArgsP, inoutVarsP, inputP, why, sizeof(why));

  if (why[0] == 0)
  {
    char* request = aasRender(requestP);

    status = aasRequest(&jP->target, "POST", "/invoke", request, &body, INVOKE_MS);
    COR_V("aas: POST %s/invoke %s: %d", jP->target.basePath, request, status);
    free(request);

    CorNode* resultP = ((status == 200) && (body != NULL)) ? aasParse(&ka, body) : NULL;

    if (status == 0)
      snprintf(why, sizeof(why), "the operation was not answered: %s", aasHttpError());
    else if (status != 200)
      snprintf(why, sizeof(why), "the server answered %d%s%.300s", status, (body != NULL) ? ": " : "", (body != NULL) ? body : "");
    else if ((resultP == NULL) || (resultP->type != CorObject))
      snprintf(why, sizeof(why), "the server's answer is no OperationResult");
    else
    {
      CorNode* successP = corTreeLookup(resultP, "success");
      CorNode* stateP   = corTreeLookup(resultP, "executionState");
      CorNode* msgsP    = corTreeLookup(resultP, "messages");
      CorNode* msgP     = ((msgsP != NULL) && (msgsP->type == CorArray)) ? msgsP->value.head : NULL;
      CorNode* textP    = ((msgP != NULL) && (msgP->type == CorObject)) ? corTreeLookup(msgP, "text") : NULL;
      bool     failed   = ((successP != NULL) && (successP->type == CorBoolean) && (successP->value.b == false)) ||
                          ((stateP != NULL) && (stateP->type == CorString) && (strcmp(stateP->value.s, "Completed") != 0));

      if (failed == true)
      {
        if ((textP != NULL) && (textP->type == CorString))
          snprintf(why, sizeof(why), "%s", textP->value.s);
        else
          snprintf(why, sizeof(why), "the operation ended %s", ((stateP != NULL) && (stateP->type == CorString)) ? stateP->value.s : "unsuccessfully");
      }
      else
      {
        CorNode* outputP = corTreeObject(&ka, NULL);

        outputAdd(&ka, outputP, corTreeLookup(resultP, "outputArguments"));
        outputAdd(&ka, outputP, corTreeLookup(resultP, "inoutputArguments"));
        output = aasRender(outputP);
      }
    }

    free(body);
  }

  if ((why[0] == 0) && (output == NULL))
    snprintf(why, sizeof(why), "out of memory");

  if (why[0] != 0)
    COR_W("aas: execution '%s' of %s:%d%s failed - %s", jP->executionId, jP->target.host, jP->target.port, jP->target.basePath, why);

  serviceEnd(jP->executionId, output, (why[0] != 0) ? why : NULL);

  free(output);
  corAllocBufferReset(&ka, false);
}



// -----------------------------------------------------------------------------
//
// invokerMain - the invocations, one at a time
//
static void* invokerMain(void* arg)
{
  (void) arg;
  sigpipeBlock();

  Job* jP;

  while ((jP = jobNext(&invocations)) != NULL)
  {
    invoke(jP);
    jobFree(jP);
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// queueDrain -
//
static void queueDrain(Queue* qP)
{
  while (qP->head != NULL)
  {
    Job* jP = qP->head;

    qP->head = jP->next;
    jobFree(jP);
  }

  qP->tail  = NULL;
  qP->count = 0;
}



// -----------------------------------------------------------------------------
//
// aasBridgeOpen -
//
int aasBridgeOpen(const AasTarget* targetP, const char* _authorization, const char* _caFile, bool _insecure, const BridgeBroker* _brokerP)
{
  target        = *targetP;
  authorization = (_authorization != NULL) ? strdup(_authorization) : NULL;
  caFile        = (_caFile != NULL) ? strdup(_caFile) : NULL;
  insecure      = _insecure;
  brokerP       = _brokerP;
  stop          = false;

  if (pthread_create(&writerThread, NULL, writerMain, NULL) != 0)
    return BRIDGE_ERR;

  if (pthread_create(&invokerThread, NULL, invokerMain, NULL) != 0)
  {
    pthread_mutex_lock(&mutex);
    stop = true;
    pthread_cond_signal(&writes.cond);
    pthread_mutex_unlock(&mutex);
    pthread_join(writerThread, NULL);
    return BRIDGE_ERR;
  }

  if (pthread_create(&pollThread, NULL, pollMain, NULL) != 0)
  {
    pthread_mutex_lock(&mutex);
    stop = true;
    pthread_cond_signal(&writes.cond);
    pthread_cond_signal(&invocations.cond);
    pthread_mutex_unlock(&mutex);
    pthread_join(writerThread, NULL);
    pthread_join(invokerThread, NULL);
    return BRIDGE_ERR;
  }

  threads = true;
  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// aasBridgeClose -
//
void aasBridgeClose(void)
{
  if (threads == true)
  {
    pthread_mutex_lock(&mutex);
    stop = true;
    pthread_cond_signal(&writes.cond);
    pthread_cond_signal(&invocations.cond);
    pthread_mutex_unlock(&mutex);

    pthread_join(pollThread, NULL);                   // after the read in hand: 10 s per request at the most
    pthread_join(writerThread, NULL);
    pthread_join(invokerThread, NULL);                // after the invocation in hand
    threads = false;
  }

  queueDrain(&writes);
  queueDrain(&invocations);

  for (int ix = 0; ix < endpoints; ix++)
    endpointFree(endpointV[ix]);

  free(endpointV);
  endpointV = NULL;
  endpoints = 0;

  free(authorization);
  free(caFile);
  authorization = NULL;
  caFile        = NULL;

  aasTargetRelease(&target);
}



// -----------------------------------------------------------------------------
//
// aasEndpointAdd -
//
int aasEndpointAdd(const char* root, BridgeDirection direction, int64_t pollMs)
{
  if (root[0] == 0)
    return BRIDGE_BAD_INPUT;

  pthread_mutex_lock(&mutex);

  Endpoint* epP = endpointFind(root);
  int       r   = BRIDGE_OK;

  if (epP != NULL)
  {
    epP->direction = direction;
    epP->pollMs    = pollMs;
  }
  else
  {
    Endpoint** v = realloc(endpointV, (endpoints + 1) * sizeof(Endpoint*));

    epP = calloc(1, sizeof(Endpoint));

    if (v != NULL)
      endpointV = v;

    if ((v == NULL) || (epP == NULL) || ((epP->root = strdup(root)) == NULL))
    {
      free(epP);
      r = BRIDGE_ERR;
    }
    else
    {
      epP->direction  = direction;
      epP->pollMs     = pollMs;
      epP->nextPoll   = 0;                            // read at once
      epP->lastStatus = -1;
      endpointV[endpoints++] = epP;
    }
  }

  pthread_mutex_unlock(&mutex);
  return r;
}



// -----------------------------------------------------------------------------
//
// aasEndpointDel -
//
int aasEndpointDel(const char* root)
{
  pthread_mutex_lock(&mutex);

  bool found = false;

  for (int ix = 0; ix < endpoints; ix++)
  {
    if (strcmp(endpointV[ix]->root, root) == 0)
    {
      endpointFree(endpointV[ix]);
      endpointV[ix] = endpointV[--endpoints];
      found         = true;
      break;
    }
  }

  pthread_mutex_unlock(&mutex);
  return (found == true) ? BRIDGE_OK : BRIDGE_NOT_FOUND;
}



// -----------------------------------------------------------------------------
//
// aasPayloadCheck -
//
int aasPayloadCheck(const char* endpoint, const char* attribute, const char* json, char* reasonBuf, int reasonBufLen)
{
  (void) attribute;

  pthread_mutex_lock(&mutex);

  AasLeaf*    leafP     = leafFind(endpoint, NULL);
  AasLeafKind kind      = (leafP != NULL) ? leafP->kind : AasLeafReadOnly;
  char*       valueType = ((leafP != NULL) && (leafP->valueType != NULL)) ? strdup(leafP->valueType) : NULL;

  pthread_mutex_unlock(&mutex);

  if (kind == AasLeafReadOnly)                        // no leaf (a container's attribute, a Relationship), or not one that is written
  {
    free(valueType);
    return BRIDGE_OK;
  }

  CorAlloc ka;
  char     kaBuf[4096];

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 16 * 1024, NULL, "aas check");

  CorNode*    valueP = aasParse(&ka, json);
  const char* why    = (valueP != NULL) ? aasValueCheck(kind, valueType, valueP, reasonBuf, reasonBufLen) : "not JSON";

  if ((why != NULL) && (why != reasonBuf))
    snprintf(reasonBuf, reasonBufLen, "%s", why);

  free(valueType);
  corAllocBufferReset(&ka, false);

  return (why == NULL) ? BRIDGE_OK : BRIDGE_BAD_INPUT;
}



// -----------------------------------------------------------------------------
//
// aasPublish -
//
int aasPublish(const char* nodeId, const char* json)
{
  pthread_mutex_lock(&mutex);

  Endpoint*   epP        = NULL;
  AasLeaf*    leafP      = leafFind(nodeId, &epP);
  bool        both       = (leafP != NULL) && (epP->direction == BridgeDirectionBoth);
  AasLeafKind kind       = (leafP != NULL) ? leafP->kind : AasLeafReadOnly;
  char*       valueType  = ((both == true) && (leafP->valueType != NULL)) ? strdup(leafP->valueType) : NULL;
  char*       submodelId = ((both == true) && (leafP->submodelId != NULL)) ? strdup(leafP->submodelId) : NULL;
  char*       path       = ((both == true) && (leafP->path != NULL)) ? strdup(leafP->path) : NULL;

  pthread_mutex_unlock(&mutex);

  int r = BRIDGE_OK;

  if (both == false)
    r = BRIDGE_NOT_FOUND;
  else if ((kind == AasLeafReadOnly) || (submodelId == NULL) || (path == NULL))
  {
    COR_W("aas: a write to '%s' refused - the element is not written back (a File, a Blob, the shell's assetInformation)", nodeId);
    r = BRIDGE_UNSUPPORTED;
  }
  else if (answered == false)
  {
    COR_W("aas: a write to '%s' refused - the server has not answered yet", nodeId);
    r = BRIDGE_ERR;
  }

  CorAlloc ka;
  char     kaBuf[4096];
  Job*     jP = NULL;

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 16 * 1024, NULL, "aas publish");

  if (r == BRIDGE_OK)
  {
    char        why[256];
    CorNode*    valueP = aasParse(&ka, json);
    const char* bad    = (valueP != NULL) ? aasValueCheck(kind, valueType, valueP, why, sizeof(why)) : "not JSON";

    if (bad != NULL)
    {
      COR_W("aas: a write to '%s' refused - %s: %s", nodeId, bad, json);
      r = BRIDGE_BAD_INPUT;
    }
    else if ((jP = calloc(1, sizeof(Job))) != NULL)
    {
      char* b64     = aasBase64Url(submodelId);
      char* pathEnc = aasPathEncode(path);
      int   len     = ((b64 != NULL) && (pathEnc != NULL)) ? (int) (strlen(b64) + strlen(pathEnc) + 64) : 0;

      jP->leafId = strdup(nodeId);
      jP->path   = (len > 0) ? malloc(len) : NULL;
      jP->body   = aasValueOnly(&ka, kind, valueP);
      jP->echo   = aasValueAsRead(&ka, kind, valueType, valueP);

      if (jP->path != NULL)
        snprintf(jP->path, len, "/submodels/%s/submodel-elements/%s/$value", b64, pathEnc);

      free(b64);
      free(pathEnc);

      if ((jP->leafId == NULL) || (jP->path == NULL) || (jP->body == NULL))
        r = BRIDGE_ERR;
    }
    else
      r = BRIDGE_ERR;
  }

  if ((r == BRIDGE_OK) && (jP != NULL))
  {
    pthread_mutex_lock(&mutex);

    bool queued = jobAdd(&writes, jP);

    if ((queued == true) && (leafFind(nodeId, &epP) != NULL))
      epP->generation++;                              // a read across the write compares no values

    pthread_mutex_unlock(&mutex);

    if (queued == false)
    {
      COR_E("aas: a write to '%s' refused - the queue is full (%d)", nodeId, QUEUE_MAX);
      r = BRIDGE_ERR;
    }
    else
      jP = NULL;
  }

  if (jP != NULL)
    jobFree(jP);

  free(valueType);
  free(submodelId);
  free(path);
  corAllocBufferReset(&ka, false);

  return r;
}



// -----------------------------------------------------------------------------
//
// aasServiceExecute -
//
int aasServiceExecute(const char* url, const char* executionId, const char* inputJson)
{
  if ((strncmp(url, "aas+http://", 11) != 0) && (strncmp(url, "aas+https://", 12) != 0))
    return BRIDGE_NOT_FOUND;

  const char* httpUrl = &url[4];

  if (strstr(httpUrl, "/submodel-elements/") == NULL)
  {
    COR_W("aas: '%s' is no Operation - aas+http[s]://<server>/submodels/<id>/submodel-elements/<idShortPath>", url);
    return BRIDGE_NOT_FOUND;
  }

  Job* jP = calloc(1, sizeof(Job));

  if (jP == NULL)
    return BRIDGE_ERR;

  //
  // Its own target, the base path the Operation's path. The server's Authorization only to the server's host
  //
  const char* why = aasTargetInit(&jP->target, httpUrl, NULL, caFile, insecure);

  if ((why == NULL) && (authorization != NULL) && (strcmp(jP->target.host, target.host) == 0) && (jP->target.port == target.port) && (jP->target.tls == target.tls))
    snprintf(jP->target.authorization, sizeof(jP->target.authorization), "%s", authorization);

  if (why != NULL)
  {
    COR_W("aas: '%s': %s", url, why);
    jobFree(jP);
    return BRIDGE_NOT_FOUND;
  }

  jP->executionId = strdup(executionId);
  jP->input       = strdup((inputJson != NULL) ? inputJson : "{}");

  if ((jP->executionId == NULL) || (jP->input == NULL))
  {
    jobFree(jP);
    return BRIDGE_ERR;
  }

  pthread_mutex_lock(&mutex);
  bool queued = jobAdd(&invocations, jP);
  pthread_mutex_unlock(&mutex);

  if (queued == false)
  {
    COR_E("aas: execution '%s' refused - the queue is full (%d)", executionId, QUEUE_MAX);
    jobFree(jP);
    return BRIDGE_ERR;
  }

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// listRead - GET /shells or /submodels: the "result" array of a page (the API pages them, cursor in
// paging_metadata) - the pages followed, up to 100. NULL: why in errBuf
//
static CorNode* listRead(CorAlloc* kaP, const char* path, int timeoutMs, char* errBuf, int errBufLen)
{
  CorNode* allP   = corTreeArray(kaP, NULL);
  char*    cursor = NULL;

  for (int page = 0; page < 100; page++)
  {
    char  url[2048];
    char* body = NULL;

    if (cursor != NULL)
      snprintf(url, sizeof(url), "%s?cursor=%s", path, cursor);
    else
      snprintf(url, sizeof(url), "%s", path);

    int      status = aasRequest(&target, "GET", url, NULL, &body, timeoutMs);
    CorNode* rootP  = ((status == 200) && (body != NULL)) ? aasParse(kaP, body) : NULL;

    free(body);

    if (status == 0)
    {
      snprintf(errBuf, errBufLen, "GET %s: %s", path, aasHttpError());
      return NULL;
    }

    if ((status != 200) || (rootP == NULL))
    {
      snprintf(errBuf, errBufLen, "GET %s: %d", path, status);
      return NULL;
    }

    CorNode* resultP = (rootP->type == CorObject) ? corTreeLookup(rootP, "result") : rootP;

    if ((resultP == NULL) || (resultP->type != CorArray))
    {
      snprintf(errBuf, errBufLen, "GET %s: no 'result' array", path);
      return NULL;
    }

    while (resultP->value.head != NULL)
    {
      CorNode* itemP = resultP->value.head;

      resultP->value.head = itemP->next;
      itemP->next         = NULL;
      corTreeChildAdd(allP, itemP);
    }

    CorNode* pagingP = (rootP->type == CorObject) ? corTreeLookup(rootP, "paging_metadata") : NULL;
    CorNode* cursorP = ((pagingP != NULL) && (pagingP->type == CorObject)) ? corTreeLookup(pagingP, "cursor") : NULL;

    if ((cursorP == NULL) || (cursorP->type != CorString) || (cursorP->value.s[0] == 0))
      break;

    char* enc = aasPathEncode(cursorP->value.s);

    cursor = (enc != NULL) ? corAllocStrdup(kaP, enc) : NULL;
    free(enc);

    if (cursor == NULL)
      break;
  }

  return allP;
}



// -----------------------------------------------------------------------------
//
// inventoryItem - a shell or a submodel as the inventory lists it: id, idShort, semanticId, submodels
//
static CorNode* inventoryItem(CorAlloc* kaP, CorNode* itemP, bool shell)
{
  CorNode* idP      = (itemP->type == CorObject) ? corTreeLookup(itemP, "id") : NULL;
  CorNode* idShortP = (idP != NULL) ? corTreeLookup(itemP, "idShort") : NULL;

  if ((idP == NULL) || (idP->type != CorString))
    return NULL;

  CorNode* outP = corTreeObject(kaP, NULL);

  corTreeChildAdd(outP, corTreeString(kaP, "id", idP->value.s));
  if ((idShortP != NULL) && (idShortP->type == CorString))
    corTreeChildAdd(outP, corTreeString(kaP, "idShort", idShortP->value.s));

  CorNode* semP  = corTreeLookup(itemP, "semanticId");
  CorNode* keysP = ((semP != NULL) && (semP->type == CorObject)) ? corTreeLookup(semP, "keys") : NULL;
  CorNode* keyP  = ((keysP != NULL) && (keysP->type == CorArray)) ? keysP->value.head : NULL;
  CorNode* valP  = ((keyP != NULL) && (keyP->type == CorObject)) ? corTreeLookup(keyP, "value") : NULL;

  if ((valP != NULL) && (valP->type == CorString))
    corTreeChildAdd(outP, corTreeString(kaP, "semanticId", valP->value.s));

  if (shell == true)
  {
    CorNode* refsP = corTreeLookup(itemP, "submodels");
    CorNode* smsP  = corTreeArray(kaP, "submodels");

    for (CorNode* refP = ((refsP != NULL) && (refsP->type == CorArray)) ? refsP->value.head : NULL; refP != NULL; refP = refP->next)
    {
      CorNode* rkeysP = (refP->type == CorObject) ? corTreeLookup(refP, "keys") : NULL;
      CorNode* firstP = ((rkeysP != NULL) && (rkeysP->type == CorArray)) ? rkeysP->value.head : NULL;
      CorNode* smIdP  = ((firstP != NULL) && (firstP->type == CorObject)) ? corTreeLookup(firstP, "value") : NULL;

      if ((smIdP != NULL) && (smIdP->type == CorString))
        corTreeChildAdd(smsP, corTreeString(kaP, NULL, smIdP->value.s));
    }

    corTreeChildAdd(outP, smsP);
  }

  return outP;
}



// -----------------------------------------------------------------------------
//
// aasInventory -
//
int aasInventory(const char* from, int depth, int timeoutMs, char** jsonP, char* errBuf, int errBufLen)
{
  (void) depth;                                       // a model is read whole: a shell is not that deep

  CorAlloc ka;
  char     kaBuf[16 * 1024];
  int      r = BRIDGE_OK;

  *jsonP = NULL;
  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 256 * 1024, NULL, "aas inventory");

  if (from != NULL)
  {
    AasRead read;
    int     status = aasModelRead(&target, &ka, from, &read, errBuf, errBufLen);

    if (status == 200)
    {
      *jsonP = aasRender(corTreeLookup(read.sampleP, "model"));
      aasReadRelease(&read);
    }
    else
      r = (status == 404) ? BRIDGE_NOT_FOUND : BRIDGE_ERR;
  }
  else
  {
    CorNode* shellsP    = listRead(&ka, "/shells", timeoutMs, errBuf, errBufLen);
    CorNode* submodelsP = (shellsP != NULL) ? listRead(&ka, "/submodels", timeoutMs, errBuf, errBufLen) : NULL;

    if (submodelsP == NULL)
      r = BRIDGE_ERR;
    else
    {
      CorNode* outP   = corTreeObject(&ka, NULL);
      CorNode* shOutP = corTreeArray(&ka, "shells");
      CorNode* smOutP = corTreeArray(&ka, "submodels");

      for (CorNode* itemP = shellsP->value.head; itemP != NULL; itemP = itemP->next)
      {
        CorNode* oneP = inventoryItem(&ka, itemP, true);

        if (oneP != NULL)
          corTreeChildAdd(shOutP, oneP);
      }

      for (CorNode* itemP = submodelsP->value.head; itemP != NULL; itemP = itemP->next)
      {
        CorNode* oneP = inventoryItem(&ka, itemP, false);

        if (oneP != NULL)
          corTreeChildAdd(smOutP, oneP);
      }

      corTreeChildAdd(outP, shOutP);
      corTreeChildAdd(outP, smOutP);
      *jsonP = aasRender(outP);
    }
  }

  if ((r == BRIDGE_OK) && (*jsonP == NULL))
  {
    snprintf(errBuf, errBufLen, "out of memory");
    r = BRIDGE_ERR;
  }

  corAllocBufferReset(&ka, false);
  return r;
}
