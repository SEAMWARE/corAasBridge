#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# aasMock.py - an AAS server's HTTP/REST API (IDTA Part 2, V3), as much of it as the aas plugin uses
#
#   GET   /shells, /submodels                                   { "result": [ ... ], "paging_metadata": {} }
#   GET   /shells/<id>, /submodels/<id>                         one; the id base64url-encoded (padded or not)
#   GET   /submodels/<id>/submodel-elements/<idShortPath>       one element
#   PATCH /submodels/<id>/submodel-elements/<idShortPath>/$value  the ValueOnly value: 204, or 400 when it does
#                                                               not fit the element (as a server checks it)
#   POST  /submodels/<id>/submodel-elements/<idShortPath>/invoke  an Operation, synchronously: an OperationResult
#
# The press's Operations: SetSpeed (speed xs:double, ramp xs:boolean -> previous xs:double), Stop (fails: the
# press is not running), Log (lines xs:string -> count xs:int). A Mode written "broken" is refused (400) - a
# value the plugin cannot know is wrong.
#
# With --authorization, a request without that Authorization header is answered 401. The test's own verbs, not
# the API's:
#
#   POST /test/set      { "submodel", "path", "value" }   an element's value changed at the source (the AAS JSON
#                                                          form: a Property's a string, an MLP's [ { language, text } ])
#   POST /test/add      { "submodel", "element" }         an element added - the model's structure changes
#   POST /test/remove   { "submodel", "idShort" }         one removed
#
# Each PATCH and each invocation is logged, one line, to the file --log names:
# "PATCH <submodel idShort>.<idShortPath> <body>", "invoke <submodel idShort>.<idShortPath> <arguments>".
#
#   aasMock.py --port <p> --log <file> [--authorization <value>] [--cert <pem> --keyfile <pem>]
#
import argparse
import base64
import json
import ssl
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

NAMEPLATE = "https://acme.example/ids/sm/press-17/nameplate"
OPERATIONAL = "https://acme.example/ids/sm/press-17/operational"
SHELL = "https://acme.example/ids/aas/press-17"


def ext(value):
  return {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": value}]}


def prop(idShort, valueType, value=None, semanticId=None):
  e = {"modelType": "Property", "idShort": idShort, "valueType": valueType}
  if value is not None:
    e["value"] = value
  if semanticId is not None:
    e["semanticId"] = ext(semanticId)
  return e


def mlp(idShort, texts, semanticId=None):
  e = {"modelType": "MultiLanguageProperty", "idShort": idShort,
       "value": [{"language": l, "text": t} for l, t in texts]}
  if semanticId is not None:
    e["semanticId"] = ext(semanticId)
  return e


def var(idShort, valueType):
  return {"value": {"modelType": "Property", "idShort": idShort, "valueType": valueType}}


SHELLS = [
  {"modelType": "AssetAdministrationShell", "id": SHELL, "idShort": "Press17",
   "assetInformation": {"assetKind": "Instance", "globalAssetId": "https://acme.example/assets/press-17"},
   "submodels": [{"type": "ModelReference", "keys": [{"type": "Submodel", "value": NAMEPLATE}]},
                 {"type": "ModelReference", "keys": [{"type": "Submodel", "value": OPERATIONAL}]}]},
  {"modelType": "AssetAdministrationShell", "id": "https://acme.example/ids/aas/press-18", "idShort": "Press18",
   "assetInformation": {"assetKind": "Instance"},
   "submodels": [{"type": "ModelReference", "keys": [{"type": "Submodel", "value": "https://acme.example/ids/sm/none"}]}]}
]

SUBMODELS = [
  {"modelType": "Submodel", "id": NAMEPLATE, "idShort": "Nameplate",
   "semanticId": ext("https://admin-shell.io/idta/nameplate/3/0/Nameplate"),
   "submodelElements": [
     mlp("ManufacturerName", [("en", "ACME Presses"), ("de", "ACME Pressen")], "0112/2///61987#ABA565#009"),
     prop("SerialNumber", "xs:string", "P17-0042", "0112/2///61987#ABA951#009"),
     prop("YearOfConstruction", "xs:int", "2024"),
     prop("NoValue", "xs:string"),
     {"modelType": "SubmodelElementCollection", "idShort": "AddressInformation", "value": [
       mlp("CityTown", [("de", "Berlin")]),
       prop("Street", "xs:string", "Hauptstr. 1")]},
     {"modelType": "SubmodelElementList", "idShort": "Markings", "typeValueListElement": "Property",
      "valueTypeListElement": "xs:string", "value": [
        {"modelType": "Property", "valueType": "xs:string", "value": "CE"},
        {"modelType": "Property", "valueType": "xs:string", "value": "UKCA"}]},
     {"modelType": "File", "idShort": "Manual", "contentType": "application/pdf", "value": "/aasx/manual.pdf"},
     {"modelType": "File", "idShort": "Logo", "contentType": "image/png", "value": "https://acme.example/logo.png"},
     {"modelType": "Blob", "idShort": "Thumbnail", "contentType": "image/png", "value": "iVBORw0KGgo="},
     {"modelType": "Capability", "idShort": "Pressing"}]},
  {"modelType": "Submodel", "id": OPERATIONAL, "idShort": "OperationalData",
   "submodelElements": [
     prop("Pressure", "xs:double", "212.5"),
     prop("Running", "xs:boolean", "true"),
     prop("Mode", "xs:string", "auto"),
     {"modelType": "Range", "idShort": "PressureRange", "valueType": "xs:double", "min": "0", "max": "400"},
     {"modelType": "ReferenceElement", "idShort": "Documentation",
      "value": {"type": "ModelReference", "keys": [{"type": "Submodel", "value": NAMEPLATE}]}},
     {"modelType": "ReferenceElement", "idShort": "Certificate", "value": ext("https://certs.example/p17")},
     {"modelType": "Operation", "idShort": "SetSpeed",
      "inputVariables": [var("speed", "xs:double"), var("ramp", "xs:boolean")],
      "outputVariables": [var("previous", "xs:double")]},
     {"modelType": "Operation", "idShort": "Stop"},
     {"modelType": "Operation", "idShort": "Log", "inputVariables": [var("lines", "xs:string")],
      "outputVariables": [var("count", "xs:int")]}]}
]

lock  = threading.Lock()
speed = [0.0]
args  = None


def b64decode(s):
  s = urllib.parse.unquote(s)
  return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4)).decode()


def byId(items, id):
  for i in items:
    if i["id"] == id:
      return i
  return None


def element(sm, path):
  # "AddressInformation.Street", "Markings[1]"
  path = urllib.parse.unquote(path)
  e = {"value": sm["submodelElements"]}
  for part in path.split("."):
    idShort, _, rest = part.partition("[")
    items = e.get("value") if isinstance(e.get("value"), list) else []
    e = next((x for x in items if x.get("idShort") == idShort), None)
    if e is None:
      return None
    while rest:
      ix, _, rest = rest.partition("]")
      rest = rest.lstrip("[")
      items = e.get("value") if isinstance(e.get("value"), list) else []
      e = items[int(ix)] if int(ix) < len(items) else None
      if e is None:
        return None
  return e


def fits(valueType, v):
  if valueType in ("xs:int", "xs:integer", "xs:long", "xs:short", "xs:byte"):
    return isinstance(v, int) and not isinstance(v, bool)
  if valueType in ("xs:double", "xs:float", "xs:decimal"):
    return isinstance(v, (int, float)) and not isinstance(v, bool)
  if valueType == "xs:boolean":
    return isinstance(v, bool)
  return isinstance(v, str)


def text(v):
  return ("true" if v else "false") if isinstance(v, bool) else str(v)


def valueSet(e, v):
  # a ValueOnly value into the element; False: it does not fit
  t = e["modelType"]
  if t == "Property":
    if not fits(e["valueType"], v) or v == "broken":
      return False
    e["value"] = text(v)
  elif t == "MultiLanguageProperty":
    if not isinstance(v, list) or not all(isinstance(x, dict) and len(x) == 1 for x in v):
      return False
    e["value"] = [{"language": l, "text": t} for x in v for l, t in x.items()]
  elif t == "Range":
    if not isinstance(v, dict) or not all(k in ("min", "max") and fits(e["valueType"], x) for k, x in v.items()):
      return False
    for k, x in v.items():
      e[k] = text(x)
  elif t == "SubmodelElementList" and e.get("typeValueListElement") == "Property":
    if not isinstance(v, list) or not all(fits(e["valueTypeListElement"], x) for x in v):
      return False
    e["value"] = [{"modelType": "Property", "valueType": e["valueTypeListElement"], "value": text(x)} for x in v]
  else:
    return False
  return True


def operation(name, inputs):
  # an OperationResult
  def out(idShort, valueType, value):
    return {"value": {"modelType": "Property", "idShort": idShort, "valueType": valueType, "value": value}}

  if name == "SetSpeed":
    previous = speed[0]
    speed[0] = float(inputs["speed"])
    return {"executionState": "Completed", "success": True, "outputArguments": [out("previous", "xs:double", repr(previous))]}
  if name == "Stop":
    return {"executionState": "Failed", "success": False, "messages": [{"messageType": "Error", "text": "the press is not running"}]}
  if name == "Log":
    return {"executionState": "Completed", "success": True, "outputArguments": [out("count", "xs:int", str(len(inputs["lines"].split(","))))]}
  return {"executionState": "Failed", "success": False}


class Handler(BaseHTTPRequestHandler):
  protocol_version = "HTTP/1.1"

  def log_message(self, fmt, *a):
    pass

  def answer(self, status, obj=None):
    body = json.dumps(obj).encode() if obj is not None else b""
    self.send_response(status)
    if obj is not None:
      self.send_header("Content-Type", "application/json")
    self.send_header("Content-Length", str(len(body)))
    self.end_headers()
    self.wfile.write(body)

  def notFound(self):
    self.answer(404, {"messages": [{"messageType": "Error", "text": "not found"}]})

  def body(self):
    n = int(self.headers.get("Content-Length", "0"))
    return self.rfile.read(n) if n > 0 else b""

  def authorized(self):
    if args.authorization is None or self.headers.get("Authorization") == args.authorization:
      return True
    self.answer(401, {"messages": [{"messageType": "Error", "text": "unauthorized"}]})
    return False

  def logLine(self, line):
    with open(args.log, "a") as f:
      f.write(line + "\n")

  def parts(self):
    return self.path.split("?")[0].strip("/").split("/")

  def do_GET(self):
    if not self.authorized():
      return
    p = self.parts()
    with lock:
      if p == ["shells"]:
        return self.answer(200, {"result": SHELLS, "paging_metadata": {}})
      if p == ["submodels"]:
        return self.answer(200, {"result": SUBMODELS, "paging_metadata": {}})
      try:
        if len(p) == 2 and p[0] in ("shells", "submodels"):
          i = byId(SHELLS if p[0] == "shells" else SUBMODELS, b64decode(p[1]))
          return self.answer(200, i) if i is not None else self.notFound()
        if len(p) == 4 and p[0] == "submodels" and p[2] == "submodel-elements":
          sm = byId(SUBMODELS, b64decode(p[1]))
          e = element(sm, p[3]) if sm is not None else None
          return self.answer(200, e) if e is not None else self.notFound()
      except (ValueError, UnicodeDecodeError):
        return self.answer(400, {"messages": [{"messageType": "Error", "text": "not base64url"}]})
    self.notFound()

  def do_PATCH(self):
    raw = self.body()
    if not self.authorized():
      return
    p = self.parts()
    if len(p) != 5 or p[0] != "submodels" or p[2] != "submodel-elements" or p[4] != "$value":
      return self.notFound()
    with lock:
      sm = byId(SUBMODELS, b64decode(p[1]))
      e = element(sm, p[3]) if sm is not None else None
      if e is None:
        return self.notFound()
      self.logLine("PATCH %s.%s %s" % (sm["idShort"], urllib.parse.unquote(p[3]), raw.decode()))
      try:
        ok = valueSet(e, json.loads(raw))
      except ValueError:
        ok = False
    if not ok:
      return self.answer(400, {"messages": [{"messageType": "Error", "text": "the value does not fit the element"}]})
    self.answer(204)

  def do_POST(self):
    raw = self.body()
    p = self.parts()

    if p[0] == "test":
      req = json.loads(raw)
      with lock:
        sm = byId(SUBMODELS, req["submodel"])
        if p == ["test", "set"]:
          element(sm, req["path"])["value"] = req["value"]
        elif p == ["test", "add"]:
          sm["submodelElements"].append(req["element"])
        elif p == ["test", "remove"]:
          sm["submodelElements"] = [e for e in sm["submodelElements"] if e.get("idShort") != req["idShort"]]
      return self.answer(200, {})

    if not self.authorized():
      return
    if len(p) != 5 or p[0] != "submodels" or p[2] != "submodel-elements" or p[4] != "invoke":
      return self.notFound()
    with lock:
      sm = byId(SUBMODELS, b64decode(p[1]))
      e = element(sm, p[3]) if sm is not None else None
      if e is None or e["modelType"] != "Operation":
        return self.notFound()
      req = json.loads(raw)
      inputs = {a["value"]["idShort"]: a["value"]["value"] for a in req.get("inputArguments", [])}
      self.logLine(" ".join(["invoke %s.%s" % (sm["idShort"], urllib.parse.unquote(p[3]))] +
                            ["%s=%s" % (k, v) for k, v in inputs.items()]))
      result = operation(e["idShort"], inputs)
    self.answer(200, result)


def main():
  global args
  p = argparse.ArgumentParser()
  p.add_argument("--port", type=int, required=True)
  p.add_argument("--log", required=True)
  p.add_argument("--authorization")
  p.add_argument("--cert")
  p.add_argument("--keyfile")
  args = p.parse_args()

  server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
  server.daemon_threads = True

  if args.cert:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(args.cert, args.keyfile)
    server.socket = ctx.wrap_socket(server.socket, server_side=True)

  server.serve_forever()


main()
