#ifndef AAS_BRIDGE_H_
#define AAS_BRIDGE_H_

//
// FILE            aasBridge.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The AAS server's shells and submodels as AssetChannels, written back, and its Operations executed.
//
// An AssetChannel's endpoint is a shell's id (or a submodel's). A thread of the plugin's own reads its model
// (aasModel.h) when the channel is added and again every pollInterval: the first read is handed over as the
// model; a read whose structure differs (an element added, a submodel removed) is the model again; else
// the leaves whose values changed, as { "values": [ ... ] } - their sourceTime the time of the read. A
// server that cannot be reached, or has no such shell, makes the channel dormant, with why.
//
// Write-back ("channelDirection": "both"): publish() is given a leaf's id and the client's value. A value
// that does not fit the element (its valueType) is refused at once; one that fits is queued, and a writer
// thread PATCHes it to .../submodel-elements/<idShortPath>/$value. A write the server refuses is logged,
// and the leaf is read back at once: the entity shows what the server has, not what was asked of it.
//
// Operations (Service Execution, the bridge an executor): a Service Registration's endpoint
// aas+http[s]://<host>[:<port>][<base path>]/submodels/<submodel id, base64url>/submodel-elements/<idShortPath>
// is an Operation; an invoker thread reads its variables, makes the execution input its input arguments by
// idShort (each one converted by its valueType), POSTs .../invoke and reports the output arguments by idShort.
//
#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // int64_t

#include "corBridge/BridgeBroker.h"                   // BridgeBroker
#include "corBridge/BridgeDriver.h"                   // BridgeDirection

#include "aasHttp.h"                                  // AasTarget



// -----------------------------------------------------------------------------
//
// aasBridgeOpen - the threads started: the poller's, the writer's and the invoker's. target is copied;
// authorization, caFile and insecure are kept for an Operation on another server than target's
//
extern int aasBridgeOpen(const AasTarget* targetP, const char* authorization, const char* caFile, bool insecure, const BridgeBroker* brokerP);



// -----------------------------------------------------------------------------
//
// aasBridgeClose - the threads stopped and joined: none calls the broker after it returns
//
extern void aasBridgeClose(void);



// -----------------------------------------------------------------------------
//
// aasEndpointAdd / Del - an AssetChannel's endpoint: a shell's or a submodel's id, read every pollMs
//
extern int aasEndpointAdd(const char* root, BridgeDirection direction, int64_t pollMs);
extern int aasEndpointDel(const char* root);



// -----------------------------------------------------------------------------
//
// aasPublish - a client's write to a leaf (its id), queued; BRIDGE_OK, BRIDGE_BAD_INPUT (the value does not
// fit the element), BRIDGE_UNSUPPORTED (an element that is not written back), BRIDGE_NOT_FOUND (no leaf of a
// write-back channel's model has the id), BRIDGE_ERR (the server has never answered, the queue is full)
//
extern int aasPublish(const char* nodeId, const char* json);



// -----------------------------------------------------------------------------
//
// aasPayloadCheck - BridgeDriver.payloadCheck: does a client's value fit the leaf's valueType?
//
extern int aasPayloadCheck(const char* endpoint, const char* attribute, const char* json, char* reasonBuf, int reasonBufLen);



// -----------------------------------------------------------------------------
//
// aasServiceExecute - BridgeDriver.serviceExecute: an Operation invoked, queued; the outcome reported by
// serviceUpdateIn
//
extern int aasServiceExecute(const char* url, const char* executionId, const char* inputJson);



// -----------------------------------------------------------------------------
//
// aasInventory - BridgeDriver.inventory: from NULL, the server's shells and submodels; from an id, that
// shell's (or submodel's) model as the node tree
//
extern int aasInventory(const char* from, int depth, int timeoutMs, char** jsonP, char* errBuf, int errBufLen);

#endif  // AAS_BRIDGE_H_
