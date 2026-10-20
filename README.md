# corAasBridge

**Prototype - not released.** An Asset Administration Shell (AAS) server in the coraine NGSI-LD broker: a
shell, its submodels and their elements as **AssetChannel** entities, a written attribute as the element's
new value, and an Operation as a service the broker executes - `aas.so`, a bridge plugin loaded with
`--bridges aas`. It speaks the AAS HTTP/REST API (IDTA Part 2, V3) as any AAS client does.

## AssetChannels

An AssetChannel's endpoint is a **shell's id** - or a submodel's. The plugin reads its model and hands it to
the broker as the neutral node tree (coraine `doc/asset-channels.md` § 3.3); the AssetChannel's mapping
(`entities`, `attributeLayout`) makes the entities - the plugin maps nothing. With `"entities": "natural"`
the shell and each submodel are an entity; the shell's submodels are Relationships of the shell.

**The read:** `GET /shells/{id}` (none: `GET /submodels/{id}`), then each submodel the shell refers to,
`GET /submodels/{id}` - each id base64url-encoded, no padding. A submodel the server does not have is no
part of the model.

| AAS | the node tree | NGSI-LD (coraine's mapping) |
|---|---|---|
| Shell | `shell`, its `assetInformation`'s `globalAssetId` and `assetKind` as two leaves | an entity, type `Shell` |
| Submodel | `submodel`, `typeName` its idShort | an entity, type its idShort |
| Property | a leaf, the value converted by its `valueType` - `xs:int` ... a number, `xs:double` ... a number, `xs:boolean` a boolean, the rest a string | Property |
| MultiLanguageProperty | a leaf, `attributeType` LanguageProperty, `{ "<language>": text }` | LanguageProperty |
| Range | a leaf, `{ "min", "max" }` converted | Property |
| File | a leaf: its URL - a path on the server (a file of the AASX package) the API's attachment URL - and `contentType` | Property, a `contentType` sub-Property |
| Blob | a leaf: the API's attachment URL, and `contentType` - the content is not copied | Property, a `contentType` sub-Property |
| ReferenceElement | a reference of the node it is in: its idShort, its target the reference's key (more keys: `<first>#<the others, '.'-joined>`) | Relationship to an entity of the model, else Property |
| SubmodelElementCollection | `collection` | by `attributeLayout` |
| SubmodelElementList | of Properties: a leaf, `attributeType` ListProperty; of references: a reference each; of anything else: a collection, its items named by their index | ListProperty, ListRelationship, by `attributeLayout` |
| Operation | `operation`, its `input` and `output` variables | no attribute - a service (below) |
| semanticId | `semanticIds` (the reference's first key) | a `semanticId` sub-Property |

Entity, RelationshipElement, AnnotatedRelationshipElement, Capability and BasicEventElement are not mapped,
nor is a Property without a value.

A leaf's id is `<submodel id>#<idShortPath>` (`AddressInformation.Street`, `Markings[0]`).

**The model read again** every `pollInterval` (`channelInfo`, an ISO 8601 duration - `PT5S`, `PT0.5S`; 10 s
by default, 0.1 s at the least), one channel at a time: a value that changed is handed over as
`{ "values": [ ... ] }`, its `sourceTime` (the attribute's `observedAt`) the time of the read; a change of
the structure (an element added or removed, a submodel gone) hands the model over again - what the model
no longer has is left alone in the broker. A server that does not answer, or has no such shell, makes the
channel dormant, with why (`GET /ngsi-ld/v1/assetChannels`); read again, it is available.

**The payload check** (bridge ABI 13): a client's write to an attribute made from a leaf is refused when it
does not fit the element - a string to an `xs:double`, 3000000000 to an `xs:int`: 400, nothing stored. On a
channel that is inbound only, too.

A Channel or a DataChannel configured for this bridge is refused (dormant): it carries AssetChannels only.

### Write-back

With `"channelDirection": "both"` (coraine's AssetChannel write-back), a client's write to an attribute
made from a leaf goes to the server first - the value, as the application's JSON, in the element's ValueOnly
form:

    PATCH /submodels/{id}/submodel-elements/{idShortPath}/$value

| element | the body |
|---|---|
| Property | the value: `300`, `"manual"`, `true` |
| MultiLanguageProperty | `[ { "en": "ACME" }, { "de": "ACME DE" } ]` - from the LanguageProperty's languageMap |
| Range | `{ "min": 10, "max": 350 }` |
| SubmodelElementList of Properties | `[ "CE", "UKCA" ]` - from the ListProperty's valueList |

- A File, a Blob and the shell's assetInformation are not written back: 422.
- The write is queued and the client answered at once; a writer thread sends the writes one at a time. Up
  to 256 wait; a full queue is refused (503), as is a write before the server has ever answered.
- A write the server refuses (400, unreachable) is logged, and the element is read back at once: the
  entity shows what the server has, not what was asked of it.
- The server's next read of a written value is no change: a write is not handed back in.

## Operations - Service Execution

An Operation is a service the bridge executes (coraine `doc/service-execution.md`, bridge ABI 11): a
Service Registration whose endpoint is

    aas+http[s]://<host>[:<port>][<base path>]/submodels/<submodel id, base64url>/submodel-elements/<idShortPath>

is the Operation. `POST /ngsi-ld/v1/entities/{id}/services/{name}` (or `POST /services`) then:

1. `GET` the Operation - its input, output and inoutput variables;
2. the execution input made its input arguments by idShort, each converted to the variable's `valueType`
   (the entity's `id` and `type`, which the broker adds, are left out). A member that is no variable, a
   variable the input does not have, a value that does not fit: the execution fails, nothing invoked;
3. `POST .../invoke` (synchronous, `clientTimeoutDuration` PT60S);
4. the OperationResult's output (and inoutput) arguments by idShort, converted by their `valueType`, are the
   execution's output. `success: false`, or an `executionState` other than `Completed`, fails it - its first
   message the reason.

A failed execution's ProblemDetails: status 502, title "The AAS operation failed", the detail why. Only
Property variables are taken. An invocation on the configured server carries its Authorization header; one
on another host does not. One invocation at a time, on a thread of its own; an invocation cannot be
cancelled.

## Inventory

`GET /admin/bridges/aas/inventory` (bridge ABI 14) - for a mapping tool:

- no `from`: the server's shells (`id`, `idShort`, their `submodels`) and submodels (`id`, `idShort`,
  `semanticId`), every page of `GET /shells` and `GET /submodels`;
- `?from=<id>`: that shell's (or submodel's) model, the node tree as it is handed over - `depth` is not
  used: a model is read whole. 404: the server has no such shell or submodel.

## Configuration

```json
{
  "aas": {
    "server": { "url": "https://aas.acme.example/api/v3.0", "authorizationEnv": "AAS_AUTHORIZATION",
                "caFile": "/etc/coraine/aas-ca.pem" },
    "ngsild": {
      "defaultEntity": false,
      "assetChannels": {
        "https://acme.example/ids/aas/press-17": {
          "entities": "natural",
          "channelDirection": "both",
          "channelInfo": [ { "key": "pollInterval", "value": "PT2S" } ] } }
    }
  }
}
```

| `server` member | |
|---|---|
| `url` | `http[s]://<host>[:<port>][/<base path>]` - where the API is (`/api/v3.0` on some servers) |
| `authorizationEnv` | the environment variable that holds the `Authorization` header's value (`Bearer ...`) - a credential is never in the file. Set and absent: the plugin is not loaded |
| `caFile` | the PEM of the CAs the server's certificate is verified against. Absent: the system's CA certificates |
| `insecure` | `true`: the certificate is not verified |

The certificate must be issued to the url's host. The plugin's TLS is its own (OpenSSL, a context of its
own): the broker's TLS settings are not touched.

## Build and test

    make && make test          # libssl-dev; the tests run python3 and the openssl tool

`test/funcTests/aasMock.py` plays the server: a shell with a Digital Nameplate and an operational-data
submodel, PATCHes and invocations logged, a value that does not fit refused, three Operations, https with a
certificate made by the test, and an Authorization header.

The broker must have AssetChannels; write-back (`COR_FEATURE_CHANNEL_WRITE_BACK`) for `"both"`, and Service
Execution for the Operations.
