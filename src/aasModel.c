//
// FILE            aasModel.c
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
// An AAS shell (or a submodel) as the neutral node tree of an AssetChannel - see header.
//
#include <errno.h>                                    // errno, ERANGE
#include <limits.h>                                   // LLONG_MAX
#include <math.h>                                     // isfinite
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // malloc, realloc, free, strtoll, strtod
#include <string.h>                                   // strcmp, strlen, strdup, memcpy

#include "corLog/corLog.h"                            // COR_W
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corAlloc/corAllocStrdup.h"                  // corAllocStrdup
#include "corJson/CorJson.h"                          // CorJson
#include "corJson/corJsonCreate.h"                    // corJsonCreate
#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corJson/corJsonRender.h"                    // corJsonFastRender
#include "corJson/corJsonRenderSize.h"                // corJsonFastRenderSize
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeObject, corTreeArray, corTreeString, corTreeChildAdd
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "aasHttp.h"                                  // AasTarget, aasRequest, aasHttpError
#include "aasModel.h"                                 // Own interface



// -----------------------------------------------------------------------------
//
// Walk - what a read builds, as it goes
//
typedef struct Walk
{
  AasTarget*   tP;
  CorAlloc*    kaP;
  AasRead*     rP;
  int          leafMax;
  bool         oom;
  const char*  submodelId;                            // the submodel being walked
  const char*  submodelB64;                           // its id, base64url
} Walk;



// -----------------------------------------------------------------------------
//
// aasBase64Url -
//
char* aasBase64Url(const char* id)
{
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

  const unsigned char* in  = (const unsigned char*) id;
  size_t               len = strlen(id);
  char*                out = malloc(len * 4 / 3 + 4);

  if (out == NULL)
    return NULL;

  char* o = out;

  for (size_t ix = 0; ix < len; ix += 3)
  {
    unsigned int n = in[ix] << 16;

    if (ix + 1 < len) n |= in[ix + 1] << 8;
    if (ix + 2 < len) n |= in[ix + 2];

    *o++ = alphabet[(n >> 18) & 63];
    *o++ = alphabet[(n >> 12) & 63];
    if (ix + 1 < len) *o++ = alphabet[(n >> 6) & 63];
    if (ix + 2 < len) *o++ = alphabet[n & 63];
  }

  *o = 0;
  return out;
}



// -----------------------------------------------------------------------------
//
// aasPathEncode -
//
char* aasPathEncode(const char* path)
{
  char* out = malloc(strlen(path) * 3 + 1);

  if (out == NULL)
    return NULL;

  char* o = out;

  for (const unsigned char* p = (const unsigned char*) path; *p != 0; p++)
  {
    bool plain = ((*p >= 'A') && (*p <= 'Z')) || ((*p >= 'a') && (*p <= 'z')) || ((*p >= '0') && (*p <= '9')) ||
                 (*p == '.') || (*p == '_') || (*p == '~') || (*p == '-');

    if (plain == true)
      *o++ = *p;
    else
      o += sprintf(o, "%%%02X", *p);
  }

  *o = 0;
  return out;
}



// -----------------------------------------------------------------------------
//
// aasRender -
//
char* aasRender(CorNode* nodeP)
{
  int   size = corJsonFastRenderSize(nodeP);
  char* json = malloc(size + 1);

  if (json != NULL)
    corJsonFastRender(nodeP, json);

  return json;
}



// -----------------------------------------------------------------------------
//
// aasParse -
//
CorNode* aasParse(CorAlloc* kaP, const char* text)
{
  CorJson cj;
  char*   copy = corAllocStrdup(kaP, text);

  if (copy == NULL)
    return NULL;

  corJsonCreate(&cj, kaP);
  return corJsonParse(&cj, copy);
}



// -----------------------------------------------------------------------------
//
// str - a string member, NULL if none (or not a string)
//
static const char* str(CorNode* objectP, const char* name)
{
  CorNode* nodeP = ((objectP != NULL) && (objectP->type == CorObject)) ? corTreeLookup(objectP, name) : NULL;

  return ((nodeP != NULL) && (nodeP->type == CorString)) ? nodeP->value.s : NULL;
}



// -----------------------------------------------------------------------------
//
// member - a member of a given type, NULL if none
//
static CorNode* member(CorNode* objectP, const char* name, CorValueType type)
{
  CorNode* nodeP = ((objectP != NULL) && (objectP->type == CorObject)) ? corTreeLookup(objectP, name) : NULL;

  return ((nodeP != NULL) && (nodeP->type == type)) ? nodeP : NULL;
}



// -----------------------------------------------------------------------------
//
// IntRange - an integer valueType's values
//
typedef struct IntRange
{
  const char*  valueType;
  long long    min;
  long long    max;
} IntRange;

static const IntRange intRanges[] =
{
  { "xs:byte",               -128,        127        },
  { "xs:short",              -32768,      32767      },
  { "xs:int",                -2147483648LL, 2147483647LL },
  { "xs:long",               LLONG_MIN,   LLONG_MAX  },
  { "xs:integer",            LLONG_MIN,   LLONG_MAX  },
  { "xs:unsignedByte",       0,           255        },
  { "xs:unsignedShort",      0,           65535      },
  { "xs:unsignedInt",        0,           4294967295LL },
  { "xs:unsignedLong",       0,           LLONG_MAX  },
  { "xs:nonNegativeInteger", 0,           LLONG_MAX  },
  { "xs:positiveInteger",    1,           LLONG_MAX  },
  { "xs:nonPositiveInteger", LLONG_MIN,   0          },
  { "xs:negativeInteger",    LLONG_MIN,   -1         }
};



// -----------------------------------------------------------------------------
//
// intRange - the integer valueType's range, NULL if it is no integer type
//
static const IntRange* intRange(const char* valueType)
{
  if (valueType == NULL)
    return NULL;

  for (unsigned int ix = 0; ix < sizeof(intRanges) / sizeof(intRanges[0]); ix++)
  {
    if (strcmp(intRanges[ix].valueType, valueType) == 0)
      return &intRanges[ix];
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// isFloatType -
//
static bool isFloatType(const char* valueType)
{
  return (valueType != NULL) && ((strcmp(valueType, "xs:double") == 0) || (strcmp(valueType, "xs:float") == 0) || (strcmp(valueType, "xs:decimal") == 0));
}



// -----------------------------------------------------------------------------
//
// isBooleanType -
//
static bool isBooleanType(const char* valueType)
{
  return (valueType != NULL) && (strcmp(valueType, "xs:boolean") == 0);
}



// -----------------------------------------------------------------------------
//
// aasTypedValue -
//
CorNode* aasTypedValue(CorAlloc* kaP, const char* name, const char* valueType, const char* text)
{
  char* end = NULL;

  if ((text[0] != 0) && (intRange(valueType) != NULL))
  {
    errno = 0;
    long long v = strtoll(text, &end, 10);

    if ((*end == 0) && (errno == 0))
      return corTreeInteger(kaP, name, v);
  }
  else if ((text[0] != 0) && (isFloatType(valueType) == true))
  {
    errno = 0;
    double d = strtod(text, &end);

    if ((*end == 0) && (errno == 0) && (isfinite(d)))
    {
      if ((d == (double) (long long) d) && (d > -9e15) && (d < 9e15))
        return corTreeInteger(kaP, name, (long long) d);    // 212 as 212, not 212.0 - JSON has one number type
      return corTreeFloat(kaP, name, d);
    }
  }
  else if (isBooleanType(valueType) == true)
  {
    if ((strcmp(text, "true") == 0) || (strcmp(text, "1") == 0))
      return corTreeBoolean(kaP, name, true);
    if ((strcmp(text, "false") == 0) || (strcmp(text, "0") == 0))
      return corTreeBoolean(kaP, name, false);
  }

  return corTreeString(kaP, name, (char*) text);      // a string type - or a value its type does not read: as it is
}



// -----------------------------------------------------------------------------
//
// aasValueText -
//
char* aasValueText(CorNode* valueP)
{
  char buf[64];

  switch (valueP->type)
  {
  case CorString:   return strdup(valueP->value.s);
  case CorBoolean:  return strdup((valueP->value.b == true) ? "true" : "false");
  case CorInt:      snprintf(buf, sizeof(buf), "%lld", valueP->value.i); return strdup(buf);
  case CorFloat:    snprintf(buf, sizeof(buf), "%.17g", valueP->value.f); return strdup(buf);
  default:          return NULL;
  }
}



// -----------------------------------------------------------------------------
//
// scalarCheck - does a scalar fit a valueType? NULL: it does
//
static const char* scalarCheck(const char* valueType, CorNode* valueP, char* whyBuf, int whyLen)
{
  const IntRange* rangeP = intRange(valueType);

  if (rangeP != NULL)
  {
    if ((valueP->type != CorInt) || (valueP->value.i < rangeP->min) || (valueP->value.i > rangeP->max))
    {
      snprintf(whyBuf, whyLen, "not an %s", valueType);
      return whyBuf;
    }

    return NULL;
  }

  if (isFloatType(valueType) == true)
  {
    if ((valueP->type != CorInt) && (valueP->type != CorFloat))
    {
      snprintf(whyBuf, whyLen, "not an %s - a number", valueType);
      return whyBuf;
    }

    return NULL;
  }

  if (isBooleanType(valueType) == true)
  {
    if (valueP->type != CorBoolean)
    {
      snprintf(whyBuf, whyLen, "not an xs:boolean - true or false");
      return whyBuf;
    }

    return NULL;
  }

  if (valueP->type != CorString)
  {
    snprintf(whyBuf, whyLen, "not an %s - a string", (valueType != NULL) ? valueType : "xs:string");
    return whyBuf;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// aasValueCheck -
//
const char* aasValueCheck(AasLeafKind kind, const char* valueType, CorNode* valueP, char* whyBuf, int whyLen)
{
  switch (kind)
  {
  case AasLeafProperty:
    return scalarCheck(valueType, valueP, whyBuf, whyLen);

  case AasLeafLanguage:
    if ((valueP->type != CorObject) || (valueP->value.head == NULL))
    {
      snprintf(whyBuf, whyLen, "not a language map - { \"<language>\": \"<text>\" }");
      return whyBuf;
    }

    for (CorNode* langP = valueP->value.head; langP != NULL; langP = langP->next)
    {
      if ((langP->type != CorString) || (langP->name[0] == 0))
      {
        snprintf(whyBuf, whyLen, "not a language map - each member a language, its text a string");
        return whyBuf;
      }
    }
    return NULL;

  case AasLeafRange:
    if (valueP->type != CorObject)
    {
      snprintf(whyBuf, whyLen, "not a range - { \"min\", \"max\" }");
      return whyBuf;
    }

    for (CorNode* endP = valueP->value.head; endP != NULL; endP = endP->next)
    {
      if ((strcmp(endP->name, "min") != 0) && (strcmp(endP->name, "max") != 0))
      {
        snprintf(whyBuf, whyLen, "not a range - '%.100s' is neither min nor max", endP->name);
        return whyBuf;
      }

      if (scalarCheck(valueType, endP, whyBuf, whyLen) != NULL)
        return whyBuf;
    }
    return NULL;

  case AasLeafList:
    if (valueP->type != CorArray)
    {
      snprintf(whyBuf, whyLen, "not a list - an array of %s", (valueType != NULL) ? valueType : "values");
      return whyBuf;
    }

    for (CorNode* itemP = valueP->value.head; itemP != NULL; itemP = itemP->next)
    {
      if (scalarCheck(valueType, itemP, whyBuf, whyLen) != NULL)
        return whyBuf;
    }
    return NULL;

  case AasLeafReadOnly:
    break;
  }

  snprintf(whyBuf, whyLen, "the element is not written back (a File, a Blob, the shell's assetInformation)");
  return whyBuf;
}



// -----------------------------------------------------------------------------
//
// aasValueOnly -
//
char* aasValueOnly(CorAlloc* kaP, AasLeafKind kind, CorNode* valueP)
{
  if (kind != AasLeafLanguage)
    return aasRender(valueP);                         // a value, a { min, max }, an array of values: as they are

  CorNode* arrayP = corTreeArray(kaP, NULL);

  for (CorNode* langP = valueP->value.head; langP != NULL; langP = langP->next)
  {
    CorNode* oneP = corTreeObject(kaP, NULL);

    corTreeChildAdd(oneP, corTreeString(kaP, langP->name, langP->value.s));
    corTreeChildAdd(arrayP, oneP);
  }

  return aasRender(arrayP);
}



// -----------------------------------------------------------------------------
//
// aasValueAsRead -
//
char* aasValueAsRead(CorAlloc* kaP, AasLeafKind kind, const char* valueType, CorNode* valueP)
{
  CorNode* readP = valueP;

  if ((kind == AasLeafProperty) || (kind == AasLeafRange) || (kind == AasLeafList))
  {
    readP = (kind == AasLeafList) ? corTreeArray(kaP, NULL) : (kind == AasLeafRange) ? corTreeObject(kaP, NULL) : NULL;

    if (kind == AasLeafProperty)
    {
      char* text = aasValueText(valueP);

      if (text == NULL)
        return NULL;

      readP = aasTypedValue(kaP, "value", valueType, corAllocStrdup(kaP, text));
      free(text);
    }
    else
    {
      for (CorNode* itemP = valueP->value.head; itemP != NULL; itemP = itemP->next)
      {
        char* text = aasValueText(itemP);

        if (text == NULL)
          return NULL;

        corTreeChildAdd(readP, aasTypedValue(kaP, (kind == AasLeafRange) ? itemP->name : NULL, valueType, corAllocStrdup(kaP, text)));
        free(text);
      }
    }
  }

  return aasRender(readP);
}



// -----------------------------------------------------------------------------
//
// leafAdd - a leaf of the read
//
static bool leafAdd(Walk* wP, const char* id, const char* path, const char* valueType, AasLeafKind kind, CorNode* valueP)
{
  AasRead* rP = wP->rP;

  if (rP->leaves == wP->leafMax)
  {
    int      max = (wP->leafMax == 0) ? 32 : wP->leafMax * 2;
    AasLeaf* v   = realloc(rP->leafV, max * sizeof(AasLeaf));

    if (v == NULL)
    {
      wP->oom = true;
      return false;
    }

    rP->leafV   = v;
    wP->leafMax = max;
  }

  AasLeaf* leafP = &rP->leafV[rP->leaves];

  memset(leafP, 0, sizeof(AasLeaf));
  leafP->id         = strdup(id);
  leafP->submodelId = (wP->submodelId != NULL) ? strdup(wP->submodelId) : NULL;
  leafP->path       = (path != NULL) ? strdup(path) : NULL;
  leafP->valueType  = (valueType != NULL) ? strdup(valueType) : NULL;
  leafP->kind       = kind;
  leafP->valueP     = valueP;
  leafP->valueJson  = aasRender(valueP);

  if ((leafP->id == NULL) || (leafP->valueJson == NULL))
  {
    aasLeafFree(leafP);
    wP->oom = true;
    return false;
  }

  rP->leaves++;
  return true;
}



// -----------------------------------------------------------------------------
//
// aasLeafFree -
//
void aasLeafFree(AasLeaf* leafP)
{
  free(leafP->id);
  free(leafP->submodelId);
  free(leafP->path);
  free(leafP->valueType);
  free(leafP->valueJson);
  memset(leafP, 0, sizeof(AasLeaf));
}



// -----------------------------------------------------------------------------
//
// aasReadRelease -
//
void aasReadRelease(AasRead* rP)
{
  for (int ix = 0; ix < rP->leaves; ix++)
    aasLeafFree(&rP->leafV[ix]);

  free(rP->leafV);
  free(rP->skeleton);
  memset(rP, 0, sizeof(AasRead));
}



// -----------------------------------------------------------------------------
//
// referenceTarget - what a Reference refers to: its one key's value; with more keys (an element of a
// submodel) "<the first>#<the others, '.'-joined>" - the leaf id the element has here
//
static char* referenceTarget(CorAlloc* kaP, CorNode* refP)
{
  CorNode* keysP = member(refP, "keys", CorArray);

  if ((keysP == NULL) || (keysP->value.head == NULL))
    return NULL;

  const char* first = str(keysP->value.head, "value");

  if (first == NULL)
    return NULL;

  if (keysP->value.head->next == NULL)
    return corAllocStrdup(kaP, first);

  int len = strlen(first) + 1;

  for (CorNode* keyP = keysP->value.head->next; keyP != NULL; keyP = keyP->next)
    len += ((str(keyP, "value") != NULL) ? strlen(str(keyP, "value")) : 0) + 1;

  char* target = corAlloc(kaP, len + 1);

  if (target == NULL)
    return NULL;

  strcpy(target, first);
  strcat(target, "#");

  for (CorNode* keyP = keysP->value.head->next; keyP != NULL; keyP = keyP->next)
  {
    if (str(keyP, "value") == NULL)
      continue;

    if (keyP != keysP->value.head->next)
      strcat(target, ".");
    strcat(target, str(keyP, "value"));
  }

  return target;
}



// -----------------------------------------------------------------------------
//
// semanticIdsAdd - an element's semanticId (the first key of the reference) as "semanticIds"
//
static void semanticIdsAdd(CorAlloc* kaP, CorNode* nodeP, CorNode* elementP)
{
  CorNode* refP  = member(elementP, "semanticId", CorObject);
  CorNode* keysP = member(refP, "keys", CorArray);

  if ((keysP == NULL) || (keysP->value.head == NULL) || (str(keysP->value.head, "value") == NULL))
    return;

  CorNode* idsP = corTreeArray(kaP, "semanticIds");

  corTreeChildAdd(idsP, corTreeString(kaP, NULL, (char*) str(keysP->value.head, "value")));
  corTreeChildAdd(nodeP, idsP);
}



// -----------------------------------------------------------------------------
//
// referenceAdd - a reference of a node: { "type", "target" }
//
static void referenceAdd(CorAlloc* kaP, CorNode* nodeP, const char* type, const char* target)
{
  CorNode* refsP = member(nodeP, "references", CorArray);

  if (refsP == NULL)
  {
    refsP = corTreeArray(kaP, "references");
    corTreeChildAdd(nodeP, refsP);
  }

  CorNode* refP = corTreeObject(kaP, NULL);

  corTreeChildAdd(refP, corTreeString(kaP, "type",   (char*) type));
  corTreeChildAdd(refP, corTreeString(kaP, "target", (char*) target));
  corTreeChildAdd(refsP, refP);
}



// -----------------------------------------------------------------------------
//
// pathJoin - "<parent>.<idShort>", or "<parent>[<index>]" (index >= 0), in kaP
//
static char* pathJoin(CorAlloc* kaP, const char* parent, const char* idShort, int index)
{
  char buf[2048];

  if (index >= 0)
    snprintf(buf, sizeof(buf), "%s[%d]", parent, index);
  else if (parent != NULL)
    snprintf(buf, sizeof(buf), "%s.%s", parent, idShort);
  else
    snprintf(buf, sizeof(buf), "%s", idShort);

  return corAllocStrdup(kaP, buf);
}



// -----------------------------------------------------------------------------
//
// leafId - "<submodel id>#<path>", in kaP
//
static char* leafId(Walk* wP, const char* path)
{
  int   len = strlen(wP->submodelId) + strlen(path) + 2;
  char* id  = corAlloc(wP->kaP, len);

  if (id != NULL)
    snprintf(id, len, "%s#%s", wP->submodelId, path);

  return id;
}



// -----------------------------------------------------------------------------
//
// attachmentUrl - where the API serves a File's or a Blob's content
//
static char* attachmentUrl(Walk* wP, const char* path)
{
  char* pathEnc = aasPathEncode(path);
  char  buf[4096];

  if (pathEnc == NULL)
    return NULL;

  snprintf(buf, sizeof(buf), "%s://%s:%d%s/submodels/%s/submodel-elements/%s/attachment",
           (wP->tP->tls == true) ? "https" : "http", wP->tP->host, wP->tP->port, wP->tP->basePath, wP->submodelB64, pathEnc);
  free(pathEnc);

  return corAllocStrdup(wP->kaP, buf);
}



// -----------------------------------------------------------------------------
//
// leafNode - a leaf: { "node": "property", "id", "name", "value", ... }
//
static CorNode* leafNode(Walk* wP, CorNode* elementP, const char* name, const char* id, CorNode* valueP)
{
  CorNode* nodeP = corTreeObject(wP->kaP, NULL);

  corTreeChildAdd(nodeP, corTreeString(wP->kaP, "node", "property"));
  corTreeChildAdd(nodeP, corTreeString(wP->kaP, "id",   (char*) id));
  corTreeChildAdd(nodeP, corTreeString(wP->kaP, "name", (char*) name));
  semanticIdsAdd(wP->kaP, nodeP, elementP);
  corTreeChildAdd(nodeP, valueP);

  return nodeP;
}



// -----------------------------------------------------------------------------
//
// languageMap - a MultiLanguageProperty's value, [ { "language", "text" } ], as { "<language>": text }
//
static CorNode* languageMap(CorAlloc* kaP, const char* name, CorNode* arrayP)
{
  CorNode* mapP = corTreeObject(kaP, name);

  for (CorNode* oneP = (arrayP != NULL) ? arrayP->value.head : NULL; oneP != NULL; oneP = oneP->next)
  {
    const char* language = str(oneP, "language");
    const char* text     = str(oneP, "text");

    if ((language != NULL) && (text != NULL))
      corTreeChildAdd(mapP, corTreeString(kaP, language, (char*) text));
  }

  return (mapP->value.head != NULL) ? mapP : NULL;
}



// -----------------------------------------------------------------------------
//
// rangeValue - a Range's { "min", "max" }, converted; NULL if it has neither
//
static CorNode* rangeValue(CorAlloc* kaP, const char* name, CorNode* elementP)
{
  const char* valueType = str(elementP, "valueType");
  const char* min       = str(elementP, "min");
  const char* max       = str(elementP, "max");

  if ((min == NULL) && (max == NULL))
    return NULL;

  CorNode* rangeP = corTreeObject(kaP, name);

  if (min != NULL)
    corTreeChildAdd(rangeP, aasTypedValue(kaP, "min", valueType, min));
  if (max != NULL)
    corTreeChildAdd(rangeP, aasTypedValue(kaP, "max", valueType, max));

  return rangeP;
}



// -----------------------------------------------------------------------------
//
// variables - an Operation's input or output variables as [ { "name", "valueType" } ]
//
static CorNode* variables(CorAlloc* kaP, const char* name, CorNode* elementP, const char* own, const char* inout)
{
  CorNode*    varsP    = corTreeArray(kaP, name);
  const char* lists[2] = { own, inout };

  for (int ix = 0; ix < 2; ix++)
  {
    CorNode* listP = member(elementP, lists[ix], CorArray);

    for (CorNode* varP = (listP != NULL) ? listP->value.head : NULL; varP != NULL; varP = varP->next)
    {
      CorNode*    valueP  = member(varP, "value", CorObject);
      const char* idShort = str(valueP, "idShort");

      if (idShort == NULL)
        continue;

      CorNode* oneP = corTreeObject(kaP, NULL);

      corTreeChildAdd(oneP, corTreeString(kaP, "name", (char*) idShort));
      if (str(valueP, "valueType") != NULL)
        corTreeChildAdd(oneP, corTreeString(kaP, "valueType", (char*) str(valueP, "valueType")));
      corTreeChildAdd(varsP, oneP);
    }
  }

  return varsP;
}



static void elementAdd(Walk* wP, CorNode* parentP, CorNode* elementP, const char* path);
static void elementsAdd(Walk* wP, CorNode* parentP, CorNode* elementsP, const char* parentPath);



// -----------------------------------------------------------------------------
//
// listAdd - a SubmodelElementList: of Properties a ListProperty, of references a reference each, of
// anything else a collection whose items are named by their index
//
static void listAdd(Walk* wP, CorNode* parentP, CorNode* elementP, const char* idShort, const char* path)
{
  CorAlloc*   kaP       = wP->kaP;
  CorNode*    itemsP    = member(elementP, "value", CorArray);
  const char* itemType  = str(elementP, "typeValueListElement");
  const char* valueType = str(elementP, "valueTypeListElement");

  if ((itemType != NULL) && (strcmp(itemType, "ReferenceElement") == 0))
  {
    for (CorNode* itemP = (itemsP != NULL) ? itemsP->value.head : NULL; itemP != NULL; itemP = itemP->next)
    {
      char* target = referenceTarget(kaP, member(itemP, "value", CorObject));

      if (target != NULL)
        referenceAdd(kaP, parentP, idShort, target);
    }
    return;
  }

  if ((itemType != NULL) && (strcmp(itemType, "Property") == 0))
  {
    CorNode* valuesP = corTreeArray(kaP, "value");

    for (CorNode* itemP = (itemsP != NULL) ? itemsP->value.head : NULL; itemP != NULL; itemP = itemP->next)
    {
      const char* text = str(itemP, "value");

      if (text != NULL)
        corTreeChildAdd(valuesP, aasTypedValue(kaP, NULL, (valueType != NULL) ? valueType : str(itemP, "valueType"), text));
    }

    char*    id    = leafId(wP, path);
    CorNode* nodeP = leafNode(wP, elementP, idShort, id, valuesP);

    corTreeChildAdd(nodeP, corTreeString(kaP, "attributeType", "ListProperty"));
    corTreeChildAdd(member(parentP, "children", CorArray), nodeP);
    leafAdd(wP, id, path, valueType, AasLeafList, valuesP);
    return;
  }

  //
  // Collections (or anything else) - a collection of the items, each one named by its index
  //
  CorNode* collP     = corTreeObject(kaP, NULL);
  CorNode* childrenP = corTreeArray(kaP, "children");

  corTreeChildAdd(collP, corTreeString(kaP, "node", "collection"));
  corTreeChildAdd(collP, corTreeString(kaP, "name", (char*) idShort));
  semanticIdsAdd(kaP, collP, elementP);
  corTreeChildAdd(collP, childrenP);

  int index = 0;

  for (CorNode* itemP = (itemsP != NULL) ? itemsP->value.head : NULL; itemP != NULL; itemP = itemP->next, index++)
  {
    char  name[16];
    char* itemPath = pathJoin(kaP, path, NULL, index);

    snprintf(name, sizeof(name), "%d", index);

    //
    // An item is an element without an idShort: a copy of it is given its index as one - its path is
    // "<list>[<index>]"
    //
    CorNode* copyP = (itemPath != NULL) ? corTreeClone(kaP, itemP) : NULL;

    if ((copyP == NULL) || (copyP->type != CorObject))
      continue;

    if (corTreeLookup(copyP, "idShort") == NULL)
      corTreeChildAdd(copyP, corTreeString(kaP, "idShort", corAllocStrdup(kaP, name)));

    elementAdd(wP, collP, copyP, itemPath);
  }

  corTreeChildAdd(member(parentP, "children", CorArray), collP);
}



// -----------------------------------------------------------------------------
//
// elementAdd - one SubmodelElement: a node in parentP's children (or a reference of parentP)
//
static void elementAdd(Walk* wP, CorNode* parentP, CorNode* elementP, const char* path)
{
  CorAlloc*   kaP       = wP->kaP;
  const char* modelType = str(elementP, "modelType");
  const char* idShort   = str(elementP, "idShort");
  CorNode*    childrenP = member(parentP, "children", CorArray);

  if ((modelType == NULL) || (idShort == NULL) || (path == NULL))
    return;

  if (strcmp(modelType, "Property") == 0)
  {
    const char* text = str(elementP, "value");

    if (text == NULL)
      return;                                         // a Property with no value: nothing to carry

    char*    id     = leafId(wP, path);
    CorNode* valueP = aasTypedValue(kaP, "value", str(elementP, "valueType"), text);

    corTreeChildAdd(childrenP, leafNode(wP, elementP, idShort, id, valueP));
    leafAdd(wP, id, path, str(elementP, "valueType"), AasLeafProperty, valueP);
  }
  else if (strcmp(modelType, "MultiLanguageProperty") == 0)
  {
    CorNode* valueP = languageMap(kaP, "value", member(elementP, "value", CorArray));

    if (valueP == NULL)
      return;

    char*    id    = leafId(wP, path);
    CorNode* nodeP = leafNode(wP, elementP, idShort, id, valueP);

    corTreeChildAdd(nodeP, corTreeString(kaP, "attributeType", "LanguageProperty"));
    corTreeChildAdd(childrenP, nodeP);
    leafAdd(wP, id, path, NULL, AasLeafLanguage, valueP);
  }
  else if (strcmp(modelType, "Range") == 0)
  {
    CorNode* valueP = rangeValue(kaP, "value", elementP);

    if (valueP == NULL)
      return;

    char* id = leafId(wP, path);

    corTreeChildAdd(childrenP, leafNode(wP, elementP, idShort, id, valueP));
    leafAdd(wP, id, path, str(elementP, "valueType"), AasLeafRange, valueP);
  }
  else if ((strcmp(modelType, "File") == 0) || (strcmp(modelType, "Blob") == 0))
  {
    const char* value = str(elementP, "value");

    if ((strcmp(modelType, "File") == 0) && (value == NULL))
      return;

    //
    // A File's URL as it is; a path on the server (a file of the AASX package), and a Blob, the API's
    // attachment URL - where its content is read
    //
    bool        url   = (value != NULL) && ((strncmp(value, "http://", 7) == 0) || (strncmp(value, "https://", 8) == 0));
    char*       text  = ((url == true) && (strcmp(modelType, "File") == 0)) ? (char*) value : attachmentUrl(wP, path);
    char*       id    = leafId(wP, path);

    if (text == NULL)
      return;

    CorNode* valueP = corTreeString(kaP, "value", text);
    CorNode* nodeP  = leafNode(wP, elementP, idShort, id, valueP);

    if (str(elementP, "contentType") != NULL)
      corTreeChildAdd(nodeP, corTreeString(kaP, "contentType", (char*) str(elementP, "contentType")));

    corTreeChildAdd(childrenP, nodeP);
    leafAdd(wP, id, path, NULL, AasLeafReadOnly, valueP);
  }
  else if (strcmp(modelType, "ReferenceElement") == 0)
  {
    char* target = referenceTarget(kaP, member(elementP, "value", CorObject));

    if (target != NULL)
      referenceAdd(kaP, parentP, idShort, target);
  }
  else if (strcmp(modelType, "SubmodelElementCollection") == 0)
  {
    CorNode* collP = corTreeObject(kaP, NULL);

    corTreeChildAdd(collP, corTreeString(kaP, "node", "collection"));
    corTreeChildAdd(collP, corTreeString(kaP, "name", (char*) idShort));
    semanticIdsAdd(kaP, collP, elementP);
    corTreeChildAdd(collP, corTreeArray(kaP, "children"));
    elementsAdd(wP, collP, member(elementP, "value", CorArray), path);
    corTreeChildAdd(childrenP, collP);
  }
  else if (strcmp(modelType, "SubmodelElementList") == 0)
    listAdd(wP, parentP, elementP, idShort, path);
  else if (strcmp(modelType, "Operation") == 0)
  {
    CorNode* opP = corTreeObject(kaP, NULL);

    corTreeChildAdd(opP, corTreeString(kaP, "node", "operation"));
    corTreeChildAdd(opP, corTreeString(kaP, "id",   leafId(wP, path)));
    corTreeChildAdd(opP, corTreeString(kaP, "name", (char*) idShort));
    semanticIdsAdd(kaP, opP, elementP);
    corTreeChildAdd(opP, variables(kaP, "input",  elementP, "inputVariables",  "inoutputVariables"));
    corTreeChildAdd(opP, variables(kaP, "output", elementP, "outputVariables", "inoutputVariables"));
    corTreeChildAdd(childrenP, opP);
  }

  //
  // Entity, RelationshipElement, AnnotatedRelationshipElement, Capability, BasicEventElement: not mapped
  //
}



// -----------------------------------------------------------------------------
//
// elementsAdd - the elements of a submodel or a collection; parentPath NULL: a submodel's own. An element
// with no idShort is a list's item, named "<parentPath>[<index>]" by listAdd
//
static void elementsAdd(Walk* wP, CorNode* parentP, CorNode* elementsP, const char* parentPath)
{
  for (CorNode* elementP = (elementsP != NULL) ? elementsP->value.head : NULL; elementP != NULL; elementP = elementP->next)
  {
    const char* idShort = str(elementP, "idShort");

    if (idShort != NULL)
      elementAdd(wP, parentP, elementP, pathJoin(wP->kaP, parentPath, idShort, -1));
  }
}



// -----------------------------------------------------------------------------
//
// get - GET path, the body parsed in kaP; the status (0: no answer, why in whyBuf)
//
static int get(Walk* wP, const char* path, CorNode** bodyPP, char* whyBuf, int whyLen)
{
  char* body   = NULL;
  int   status = aasRequest(wP->tP, "GET", path, NULL, &body, 10000);

  *bodyPP = NULL;

  if (status == 0)
    snprintf(whyBuf, whyLen, "GET %.200s: %.200s", path, aasHttpError());
  else if (status != 200)
    snprintf(whyBuf, whyLen, "GET %.200s: %d", path, status);
  else if ((body == NULL) || ((*bodyPP = aasParse(wP->kaP, body)) == NULL) || ((*bodyPP)->type != CorObject))
  {
    snprintf(whyBuf, whyLen, "GET %.200s: the answer is no JSON object", path);
    status   = 502;
    *bodyPP  = NULL;
  }

  free(body);
  return status;
}



// -----------------------------------------------------------------------------
//
// submodelNode - a submodel as a node
//
static CorNode* submodelNode(Walk* wP, CorNode* submodelP, const char* b64)
{
  CorAlloc*   kaP     = wP->kaP;
  const char* id      = str(submodelP, "id");
  const char* idShort = str(submodelP, "idShort");

  if (id == NULL)
    return NULL;

  CorNode* nodeP = corTreeObject(kaP, NULL);

  corTreeChildAdd(nodeP, corTreeString(kaP, "node", "submodel"));
  corTreeChildAdd(nodeP, corTreeString(kaP, "id",   (char*) id));
  if (idShort != NULL)
  {
    corTreeChildAdd(nodeP, corTreeString(kaP, "name",     (char*) idShort));
    corTreeChildAdd(nodeP, corTreeString(kaP, "typeName", (char*) idShort));
  }
  semanticIdsAdd(kaP, nodeP, submodelP);
  corTreeChildAdd(nodeP, corTreeArray(kaP, "children"));

  wP->submodelId  = id;
  wP->submodelB64 = b64;
  elementsAdd(wP, nodeP, member(submodelP, "submodelElements", CorArray), NULL);
  wP->submodelId  = NULL;
  wP->submodelB64 = NULL;

  return nodeP;
}



// -----------------------------------------------------------------------------
//
// shellNode - a shell as a node: its submodels each read, its assetInformation two leaves
//
static int shellNode(Walk* wP, CorNode* shellP, CorNode** nodePP, char* whyBuf, int whyLen)
{
  CorAlloc*   kaP     = wP->kaP;
  const char* id      = str(shellP, "id");
  const char* idShort = str(shellP, "idShort");

  if (id == NULL)
  {
    snprintf(whyBuf, whyLen, "the shell has no id");
    return 502;
  }

  CorNode* nodeP     = corTreeObject(kaP, NULL);
  CorNode* childrenP = corTreeArray(kaP, "children");

  corTreeChildAdd(nodeP, corTreeString(kaP, "node", "shell"));
  corTreeChildAdd(nodeP, corTreeString(kaP, "id",   (char*) id));
  if (idShort != NULL)
    corTreeChildAdd(nodeP, corTreeString(kaP, "name", (char*) idShort));
  semanticIdsAdd(kaP, nodeP, shellP);
  corTreeChildAdd(nodeP, childrenP);

  //
  // The asset: globalAssetId and assetKind, read only
  //
  CorNode* assetP = member(shellP, "assetInformation", CorObject);

  for (int ix = 0; ix < 2; ix++)
  {
    const char* name  = (ix == 0) ? "globalAssetId" : "assetKind";
    const char* value = str(assetP, name);

    if (value == NULL)
      continue;

    int   len  = strlen(id) + strlen(name) + 2;
    char* lid  = corAlloc(kaP, len);

    if (lid == NULL)
      break;

    snprintf(lid, len, "%s#%s", id, name);

    CorNode* leafP  = corTreeObject(kaP, NULL);
    CorNode* valueP = corTreeString(kaP, "value", (char*) value);

    corTreeChildAdd(leafP, corTreeString(kaP, "node", "property"));
    corTreeChildAdd(leafP, corTreeString(kaP, "id",   lid));
    corTreeChildAdd(leafP, corTreeString(kaP, "name", (char*) name));
    corTreeChildAdd(leafP, valueP);
    corTreeChildAdd(childrenP, leafP);
    leafAdd(wP, lid, NULL, NULL, AasLeafReadOnly, valueP);
  }

  //
  // The submodels: [ { "type": "ModelReference", "keys": [ { "type": "Submodel", "value": <id> } ] } ]
  //
  CorNode* refsP = member(shellP, "submodels", CorArray);

  for (CorNode* refP = (refsP != NULL) ? refsP->value.head : NULL; refP != NULL; refP = refP->next)
  {
    CorNode*    keysP = member(refP, "keys", CorArray);
    const char* smId  = ((keysP != NULL) && (keysP->value.head != NULL)) ? str(keysP->value.head, "value") : NULL;

    if (smId == NULL)
      continue;

    char* b64  = aasBase64Url(smId);
    char  path[4096];

    if (b64 == NULL)
    {
      wP->oom = true;
      break;
    }

    snprintf(path, sizeof(path), "/submodels/%s", b64);

    CorNode* submodelP = NULL;
    int      status    = get(wP, path, &submodelP, whyBuf, whyLen);

    if (status == 404)
    {
      COR_W("aas: shell '%s' refers to submodel '%s' - the server has no such submodel", id, smId);
      free(b64);
      continue;                                       // a reference to nothing: no part of the model
    }

    if (status != 200)
    {
      free(b64);
      return status;
    }

    char* b64Kept = corAllocStrdup(kaP, b64);
    free(b64);

    CorNode* smNodeP = submodelNode(wP, submodelP, b64Kept);

    if (smNodeP != NULL)
      corTreeChildAdd(childrenP, smNodeP);
  }

  *nodePP = nodeP;
  return 200;
}



// -----------------------------------------------------------------------------
//
// Skeleton - a growing string
//
typedef struct Skeleton
{
  char*  buf;
  int    len;
  int    size;
  bool   oom;
} Skeleton;



// -----------------------------------------------------------------------------
//
// skeletonAdd - text to the skeleton
//
static void skeletonAdd(Skeleton* sP, const char* text)
{
  int n = strlen(text);

  if (sP->len + n + 1 > sP->size)
  {
    int   size = (sP->size == 0) ? 4096 : sP->size * 2;

    while (sP->len + n + 1 > size)
      size *= 2;

    char* b = realloc(sP->buf, size);

    if (b == NULL)
    {
      sP->oom = true;
      return;
    }

    sP->buf  = b;
    sP->size = size;
  }

  memcpy(&sP->buf[sP->len], text, n + 1);
  sP->len += n;
}



// -----------------------------------------------------------------------------
//
// skeletonOf - a node and all below it, without the leaves' values
//
static void skeletonOf(Skeleton* sP, CorNode* nodeP)
{
  skeletonAdd(sP, "{");

  for (CorNode* memberP = nodeP->value.head; memberP != NULL; memberP = memberP->next)
  {
    if (strcmp(memberP->name, "value") == 0)
      continue;

    skeletonAdd(sP, memberP->name);
    skeletonAdd(sP, "=");

    if (strcmp(memberP->name, "children") == 0)
    {
      for (CorNode* childP = memberP->value.head; childP != NULL; childP = childP->next)
        skeletonOf(sP, childP);
    }
    else
    {
      char* json = aasRender(memberP);

      if (json == NULL)
        sP->oom = true;
      else
      {
        skeletonAdd(sP, json);
        free(json);
      }
    }

    skeletonAdd(sP, ";");
  }

  skeletonAdd(sP, "}");
}



// -----------------------------------------------------------------------------
//
// aasModelRead -
//
int aasModelRead(AasTarget* tP, CorAlloc* kaP, const char* rootId, AasRead* rP, char* whyBuf, int whyLen)
{
  Walk  walk = { tP, kaP, rP, 0, false, NULL, NULL };
  char* b64  = aasBase64Url(rootId);
  char  path[4096];

  memset(rP, 0, sizeof(AasRead));
  whyBuf[0] = 0;

  if (b64 == NULL)
  {
    snprintf(whyBuf, whyLen, "out of memory");
    return 500;
  }

  CorNode* rootP  = NULL;
  CorNode* bodyP  = NULL;
  int      status;

  snprintf(path, sizeof(path), "/shells/%s", b64);
  status = get(&walk, path, &bodyP, whyBuf, whyLen);

  if (status == 200)
    status = shellNode(&walk, bodyP, &rootP, whyBuf, whyLen);
  else if (status == 404)
  {
    snprintf(path, sizeof(path), "/submodels/%s", b64);
    status = get(&walk, path, &bodyP, whyBuf, whyLen);

    if (status == 200)
    {
      rootP = submodelNode(&walk, bodyP, corAllocStrdup(kaP, b64));

      if (rootP == NULL)
      {
        snprintf(whyBuf, whyLen, "the submodel has no id");
        status = 502;
      }
    }
    else if (status == 404)
      snprintf(whyBuf, whyLen, "the server has no shell and no submodel '%.200s'", rootId);
  }

  free(b64);

  if ((status == 200) && (walk.oom == true))
  {
    snprintf(whyBuf, whyLen, "out of memory");
    status = 500;
  }

  if (status != 200)
  {
    aasReadRelease(rP);
    return status;
  }

  Skeleton skeleton = { NULL, 0, 0, false };

  skeletonOf(&skeleton, rootP);

  if (skeleton.oom == true)
  {
    free(skeleton.buf);
    aasReadRelease(rP);
    snprintf(whyBuf, whyLen, "out of memory");
    return 500;
  }

  rP->skeleton = skeleton.buf;
  rP->sampleP  = corTreeObject(kaP, NULL);
  rootP->name  = (char*) "model";
  corTreeChildAdd(rP->sampleP, rootP);

  return 200;
}
