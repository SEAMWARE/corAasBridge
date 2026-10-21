#ifndef AAS_MODEL_H_
#define AAS_MODEL_H_

//
// FILE            aasModel.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// An AAS shell (or a submodel) as the neutral node tree of an AssetChannel (coraine's doc/asset-channels.md
// § 3.3): read from the server through the AAS HTTP API - GET /shells/{id}, then each submodel it refers
// to, GET /submodels/{id}, the ids base64url-encoded - and each element made a node:
//
//   Shell                       { "node": "shell", "id", "name": idShort, "semanticIds", "children": its submodels,
//                                 and its assetInformation's globalAssetId and assetKind as two leaves }
//   Submodel                    { "node": "submodel", "id", "name": idShort, "typeName": idShort, "children" }
//   Property                    a leaf, the value converted by its valueType (a number, a boolean, a string)
//   MultiLanguageProperty       a leaf, "attributeType": "LanguageProperty", the value { "<language>": text }
//   Range                       a leaf, the value { "min", "max" } converted
//   File                        a leaf, the value its URL (a path on the server: the API's attachment URL) and
//                               its "contentType"
//   Blob                        a leaf, the value the API's attachment URL, and its "contentType" - the content
//                               itself is not copied
//   ReferenceElement            a reference of the node it is in: { "type": idShort, "target": its last key }
//   SubmodelElementCollection   { "node": "collection", "name": idShort, "children" }
//   SubmodelElementList         of values: a leaf, "attributeType": "ListProperty", the values; of references: a
//                               reference each (a ListRelationship); of anything else: a collection, its items
//                               named by their index
//   Operation                   { "node": "operation", "id", "name", "input", "output" } - a service (aasService.h)
//   Entity, RelationshipElement, AnnotatedRelationshipElement, Capability, BasicEventElement: not mapped
//
// A leaf's "id" is "<submodel id>#<idShortPath>" (Unit.Pressure, Markings[0]): what a value change, and a
// client's write back, name.
//
#include <stdbool.h>                                  // bool

#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corTree/CorNode.h"                          // CorNode

#include "aasHttp.h"                                  // AasTarget



// -----------------------------------------------------------------------------
//
// AasLeafKind - what a leaf is at the server, for a write back
//
typedef enum AasLeafKind
{
  AasLeafProperty,                                    // $value: the value, typed by valueType
  AasLeafLanguage,                                    // $value: [ { "<language>": text }, ... ]
  AasLeafRange,                                       // $value: { "min", "max" }
  AasLeafList,                                        // $value: [ value, ... ] of one valueType
  AasLeafReadOnly                                     // not written: a File, a Blob, the shell's assetInformation
} AasLeafKind;



// -----------------------------------------------------------------------------
//
// AasLeaf - one leaf of a model as read
//
typedef struct AasLeaf
{
  char*        id;                                    // "<submodel id>#<idShortPath>"
  char*        submodelId;
  char*        path;                                  // idShortPath
  char*        valueType;                             // "xs:double", ...; NULL: none (a language map)
  AasLeafKind  kind;
  char*        valueJson;                             // the value, rendered - what a change is told by; NULL: unknown
  CorNode*     valueP;                                // during a read only: the value in the read's tree
} AasLeaf;



// -----------------------------------------------------------------------------
//
// AasRead - a model, read
//
typedef struct AasRead
{
  CorNode*  sampleP;                                  // { "model": <the root node> } - in the read's CorAlloc
  AasLeaf*  leafV;                                    // malloc'd, and each leaf's strings
  int       leaves;
  char*     skeleton;                                 // the structure, without the values - what a change of it is told by
} AasRead;



// -----------------------------------------------------------------------------
//
// aasModelRead - the model of rootId (a shell's id, or a submodel's) read from the server, its tree allocated
// in kaP. 200: read; 404: the server has neither a shell nor a submodel of that id; 0: no answer; else the
// status the server answered. Anything but 200: why, in whyBuf
//
extern int aasModelRead(AasTarget* tP, CorAlloc* kaP, const char* rootId, AasRead* rP, char* whyBuf, int whyLen);



// -----------------------------------------------------------------------------
//
// aasReadRelease - the leaves and the skeleton freed (the tree is the CorAlloc's)
//
extern void aasReadRelease(AasRead* rP);



// -----------------------------------------------------------------------------
//
// aasLeafFree - one leaf's strings
//
extern void aasLeafFree(AasLeaf* leafP);



// -----------------------------------------------------------------------------
//
// aasValueCheck - does valueP (a client's value, as the application's own JSON) fit a leaf? NULL: it does;
// else why not
//
extern const char* aasValueCheck(AasLeafKind kind, const char* valueType, CorNode* valueP, char* whyBuf, int whyLen);



// -----------------------------------------------------------------------------
//
// aasValueOnly - the body of PATCH .../$value for a value that fits (the AAS ValueOnly form), malloc'd
//
extern char* aasValueOnly(CorAlloc* kaP, AasLeafKind kind, CorNode* valueP);



// -----------------------------------------------------------------------------
//
// aasValueAsRead - a value that fits, as the next read of the leaf renders it once the server has it (what
// a write's echo is told by), malloc'd
//
extern char* aasValueAsRead(CorAlloc* kaP, AasLeafKind kind, const char* valueType, CorNode* valueP);



// -----------------------------------------------------------------------------
//
// aasTypedValue - an AAS value (a string in the AAS JSON) converted by its valueType: a number, a boolean,
// else the string. name: the node's name (NULL in an array)
//
extern CorNode* aasTypedValue(CorAlloc* kaP, const char* name, const char* valueType, const char* text);



// -----------------------------------------------------------------------------
//
// aasValueText - a client's scalar value as the AAS JSON writes it (a string): malloc'd, NULL if no scalar
//
extern char* aasValueText(CorNode* valueP);



// -----------------------------------------------------------------------------
//
// aasBase64Url - an id as a path segment of the API: base64url, no padding. malloc'd
//
extern char* aasBase64Url(const char* id);



// -----------------------------------------------------------------------------
//
// aasPathEncode - an idShortPath as a path segment: '[' and ']' (and anything outside [A-Za-z0-9._~-])
// percent-encoded. malloc'd
//
extern char* aasPathEncode(const char* path);



// -----------------------------------------------------------------------------
//
// aasRender - a tree as JSON text, malloc'd
//
extern char* aasRender(CorNode* nodeP);



// -----------------------------------------------------------------------------
//
// aasParse - text (copied into kaP: the parser writes into it) parsed; NULL if it is no JSON
//
extern CorNode* aasParse(CorAlloc* kaP, const char* text);

#endif  // AAS_MODEL_H_
