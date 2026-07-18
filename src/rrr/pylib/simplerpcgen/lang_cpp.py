from simplerpcgen.misc import SourceFile

# optional archive overload emission.
#
# When `archive` is True (controlled by the `--archive` CLI flag on
# rpcgen), `emit_struct` and `emit_marshaled_typed_struct` emit
# additional `BinaryWriteArchive&` / `BinaryReadArchive&`
# operator<</>> overloads alongside the existing `Marshal&` ones.
# Both forms compile and produce byte-identical wire output; Phase 4 will migrate per-command-type call sites to
# use the archive form.
#
# Default is off — generators run with the same behavior as before
# the flag was introduced. This keeps `rcc_rpc.h` (which uses
# arbitrary user-defined types like `MarshallDeputy` and `Value` that
# don't yet have archive operators) compiling without changes.

def emit_struct(struct, f, archive=False):
    f.writeln("struct %s {" % struct.name)
    with f.indent():
        for field in struct.fields:
            f.writeln("%s %s;" % (field.type, field.name))
    f.writeln("};")
    f.writeln()
    # dropped the legacy
    # `rrr::Marshal& operator>>(rrr::Marshal&, T&)` emission too.
    # 1 flipped all rpcgen dispatcher reads to
    # `BinaryReadArchive`; the routed
    # `operator>>(rusty::RefMut<Marshal>&, U&)` in `client.hpp` also
    # dispatches through the archive layer, so hand-written
    # `fu->get_reply() >> userStruct` calls route to
    # `operator>>(BinaryReadArchive&, T&)` (the archive emission
    # below).  No code path remains that would call the Marshal&
    # version on a user/typed struct.  Pairs with Phase 3e-2's
    # earlier removal of the `Marshal& operator<<` emission.
    if archive:
        # Phase 8: the serde free functions own the wire format; the
        # operators are one-line forwarders kept only until the operator
        # layer is deleted. serialize/deserialize live in the struct's own
        # namespace so ADL finds them; fields route through the qualified
        # serde namespace (specific overload when one exists, generic
        # catch-all otherwise). Byte layout identical to the old inline
        # field-by-field operators.
        f.writeln("inline void serialize(const %s& o, rrr::BinaryWriteArchive& ar) {" % struct.name)
        with f.indent():
            for field in struct.fields:
                f.writeln("rrr::Serialize_::serialize(o.%s, ar);" % field.name)
        f.writeln("}")
        f.writeln()
        f.writeln("inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const %s& o) { serialize(o, ar); return ar; }" % struct.name)
        f.writeln()
        f.writeln("inline void deserialize(%s& o, rrr::BinaryReadArchive& ar) {" % struct.name)
        with f.indent():
            for field in struct.fields:
                f.writeln("rrr::Deserialize_::deserialize(o.%s, ar);" % field.name)
        f.writeln("}")
        f.writeln()
        f.writeln("inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, %s& o) { deserialize(o, ar); return ar; }" % struct.name)
        f.writeln()

def typed_struct_name(func_name, suffix):
    # Use a distinct rpcgen prefix to avoid shadowing user/domain types
    # in derived service implementations (for example SyncLogResponse).
    parts = [part for part in func_name.split("_") if len(part) > 0]
    if len(parts) == 0:
        stem = "%s%s" % (func_name[:1].upper(), func_name[1:])
    else:
        stem = "".join(["%s%s" % (part[:1].upper(), part[1:]) for part in parts])
    return "Rpc%s%s" % (stem, suffix)

def typed_request_struct_name(func):
    return typed_struct_name(func.name, "Request")

def typed_response_struct_name(func):
    return typed_struct_name(func.name, "Response")

def typed_result_type(func):
    return "rusty::Result<%s, rrr::i32>" % typed_response_struct_name(func)

def typed_proxy_future_wrapper_name(func):
    return "%sTypedFuture" % func.name

def typed_proxy_future_result_type(func):
    return "rusty::Result<%s, rrr::i32>" % typed_proxy_future_wrapper_name(func)

def typed_struct_fields(args, fallback_prefix):
    fields = []
    for idx, arg in enumerate(args):
        if arg.name != None:
            field_name = arg.name
        else:
            field_name = "%s_%d" % (fallback_prefix, idx)
        fields += (arg.type, field_name),
    return fields

def emit_marshaled_typed_struct(struct_name, fields, f, archive=False):
    f.writeln("struct %s {" % struct_name)
    with f.indent():
        for field_type, field_name in fields:
            f.writeln("%s %s;" % (field_type, field_name))
    f.writeln("};")
    # dropped the legacy `Marshal&` `>>`
    # emission too.  Phase 3g-1 flipped dispatcher reads to
    # `BinaryReadArchive`, and the routed
    # `operator>>(rusty::RefMut<Marshal>&, U&)` overload now also
    # dispatches via the archive layer — so the auto-generated
    # whole-struct `Marshal&` `>>` overload has no remaining
    # callers.  The archive `>>` emission below is the only one
    # rpcgen needs to provide.
    if archive:
        # Phase 8: friend serde functions own the wire format (fields routed
        # through the qualified serde namespace); the friend operators are
        # one-line forwarders kept until the operator layer is deleted.
        f.writeln("friend inline void serialize(const %s& o, rrr::BinaryWriteArchive& ar) {" % struct_name)
        with f.indent():
            for _, field_name in fields:
                f.writeln("rrr::Serialize_::serialize(o.%s, ar);" % field_name)
        f.writeln("}")
        f.writeln("friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const %s& o) { serialize(o, ar); return ar; }" % struct_name)
        f.writeln("friend inline void deserialize(%s& o, rrr::BinaryReadArchive& ar) {" % struct_name)
        with f.indent():
            for _, field_name in fields:
                f.writeln("rrr::Deserialize_::deserialize(o.%s, ar);" % field_name)
        f.writeln("}")
        f.writeln("friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, %s& o) { deserialize(o, ar); return ar; }" % struct_name)
    f.writeln()

def emit_typed_service_signature(service, func, f):
    request_struct_name = typed_request_struct_name(func)
    response_struct_name = typed_response_struct_name(func)
    result_type = typed_result_type(func)
    async_result_type = "rusty::Task<%s>" % result_type

    if func.attr == "defer":
        f.writeln("// @safe")
        if service.abstract or func.abstract:
            f.writeln("virtual void %s(const %s& req, %s& resp, rrr::DeferredReply defer) = 0;" % (
                func.name,
                request_struct_name,
                response_struct_name,
            ))
        else:
            f.writeln("virtual void %s(const %s& req, %s& resp, rrr::DeferredReply defer);" % (
                func.name,
                request_struct_name,
                response_struct_name,
            ))
        return

    if func.attr == "async":
        f.writeln("// @safe")
        if service.abstract or func.abstract:
            f.writeln("virtual %s %s(const %s& req) = 0;" % (async_result_type, func.name, request_struct_name))
            return
        f.writeln("virtual %s %s(const %s& req);" % (async_result_type, func.name, request_struct_name))
        return

    f.writeln("// @safe")
    if service.abstract or func.abstract:
        f.writeln("virtual %s %s(const %s& req) = 0;" % (result_type, func.name, request_struct_name))
        return

    f.writeln("virtual %s %s(const %s& req);" % (result_type, func.name, request_struct_name))

def emit_typed_proxy_sync_signature(func, f):
    request_struct_name = typed_request_struct_name(func)
    result_type = typed_result_type(func)

    f.writeln("%s %s(const %s& req) {" % (result_type, func.name, request_struct_name))
    with f.indent():
        f.writeln("auto __typed_fu_result__ = this->async_%s(req);" % func.name)
        f.writeln("if (__typed_fu_result__.is_err()) {")
        with f.indent():
            f.writeln("return %s::Err(__typed_fu_result__.unwrap_err());" % result_type)
        f.writeln("}")
        f.writeln("return __typed_fu_result__.unwrap().resolve();")
    f.writeln("}")

def emit_proxy_request_call(service, func, marshal_args, f):
    # emit a `BinaryWriteArchive&` lambda
    # for the proxy request.  Phase 3d-2's dual-signature
    # `request_via_channel<F>` accepts either signature; archive is
    # the path forward (decouples user code from the legacy `Marshal`
    # buffer).  User-struct `operator<<` overloads have been emitted
    # for both `Marshal&` and `BinaryWriteArchive&` since Phase 3c, so
    # `__m__ << field` keeps compiling regardless of which signature
    # the lambda uses.
    if len(marshal_args) > 0:
        f.writeln("return __cl__->request(%sService::%s, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {" % (service.name, func.name.upper()))
        with f.indent():
            for arg in marshal_args:
                f.writeln("rrr::Serialize_::serialize(%s, __m__);" % arg)
        f.writeln("});")
    else:
        # Always emit the 3-arg `request(rpc_id, attr, write_fn)` form,
        # even when the RPC has no input fields (use an empty lambda).
        # This collapses Client::request's overload set toward a single
        # canonical 3-arg method, which is what the inline-Rust DSL
        # needs (Rust has no method overloading). The empty-lambda case
        # was previously emitted as `request(rpc_id, attr)` (the no-fn
        # overload), the only deptran consumer of that overload.
        f.writeln("return __cl__->request(%sService::%s, __fu_attr__, [](rrr::BinaryWriteArchive&) {});" % (service.name, func.name.upper()))

def emit_typed_proxy_future_wrapper(func, f):
    wrapper_name = typed_proxy_future_wrapper_name(func)
    response_struct_name = typed_response_struct_name(func)
    result_type = typed_result_type(func)
    output_fields = typed_struct_fields(func.output, "out")

    f.writeln("class %s {" % wrapper_name)
    f.writeln("private:")
    with f.indent():
        f.writeln("rusty::Arc<rrr::Future> __fu__;")
    f.writeln("public:")
    with f.indent():
        f.writeln("explicit %s(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }" % wrapper_name)
        f.writeln("bool ready() const {")
        with f.indent():
            f.writeln("return __fu__->ready();")
        f.writeln("}")
        f.writeln("void wait() const {")
        with f.indent():
            f.writeln("__fu__->wait();")
        f.writeln("}")
        f.writeln("rrr::i32 get_error_code() const {")
        with f.indent():
            f.writeln("return __fu__->get_error_code();")
        f.writeln("}")
        f.writeln("rusty::Arc<rrr::Future> raw_future() const {")
        with f.indent():
            f.writeln("return __fu__;")
        f.writeln("}")
        f.writeln("%s resolve() const {" % result_type)
        with f.indent():
            f.writeln("rrr::i32 __ret__ = __fu__->get_error_code();")
            f.writeln("if (__ret__ != 0) {")
            with f.indent():
                f.writeln("return %s::Err(__ret__);" % result_type)
            f.writeln("}")
            f.writeln("%s __typed_resp__;" % response_struct_name)
            # decode reply bytes through
            # BinaryReadArchive (matches the request-side archive
            # emission).  The borrow guard `__reply_guard__` keeps the
            # underlying `Marshal` alive for the entire chain of reads
            # — `__reply_src__` and `__reply_ar__` reference into it.
            if len(output_fields) > 0:
                f.writeln("auto __reply_guard__ = __fu__->get_reply();")
                f.writeln("rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));")
                for _, field_name in output_fields:
                    f.writeln("rrr::Deserialize_::deserialize(__typed_resp__.%s, __reply_ar__);" % field_name)
            f.writeln("return %s::Ok(__typed_resp__);" % result_type)
        f.writeln("}")
        f.writeln("auto operator co_await() const {")
        with f.indent():
            f.writeln("return rrr::make_typed_future_awaitable(*this);")
        f.writeln("}")
    f.writeln("};")

def emit_typed_proxy_async_signature(service, func, typed_async_call_params, f):
    request_struct_name = typed_request_struct_name(func)
    wrapper_name = typed_proxy_future_wrapper_name(func)
    result_type = typed_proxy_future_result_type(func)

    f.writeln("%s async_%s(const %s& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {" % (result_type, func.name, request_struct_name))
    with f.indent():
        if len(typed_async_call_params) > 0:
            # see `emit_proxy_request_call`.
            f.writeln("auto __fu_result__ = __cl__->request(%sService::%s, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {" % (service.name, func.name.upper()))
            with f.indent():
                for param in typed_async_call_params:
                    f.writeln("rrr::Serialize_::serialize(%s, __m__);" % param)
            f.writeln("});")
        else:
            # Empty-input case — always emit the 3-arg form with an
            # empty lambda, matching `emit_proxy_request_call`. Keeps
            # the no-fn `Client::request(rpc_id, attr)` overload
            # un-consumed, which is what the inline-Rust DSL needs.
            f.writeln("auto __fu_result__ = __cl__->request(%sService::%s, __fu_attr__, [](rrr::BinaryWriteArchive&) {});" % (service.name, func.name.upper()))
        f.writeln("if (__fu_result__.is_err()) {")
        with f.indent():
            f.writeln("return %s::Err(__fu_result__.unwrap_err());" % result_type)
        f.writeln("}")
        if len(typed_async_call_params) == 0:
            f.writeln("(void)req;")
        f.writeln("return %s::Ok(%s(__fu_result__.unwrap()));" % (result_type, wrapper_name))
    f.writeln("}")

def emit_typed_proxy_await_signature(func, f):
    request_struct_name = typed_request_struct_name(func)
    wrapper_name = typed_proxy_future_wrapper_name(func)

    f.writeln("rrr::TypedFutureResultAwaiter<%s> await_%s(const %s& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {" % (wrapper_name, func.name, request_struct_name))
    with f.indent():
        f.writeln("return rrr::make_typed_future_result_awaitable(this->async_%s(req, __fu_attr__));" % func.name)
    f.writeln("}")

def emit_service_and_proxy(service, f, rpc_table, archive=False):
    f.writeln("class %sService {" % service.name)
    f.writeln("public:")
    with f.indent():
        f.writeln("// Typed request/response scaffolding generated from RPC signature lists.")
        for func in service.functions:
            request_struct_name = typed_request_struct_name(func)
            response_struct_name = typed_response_struct_name(func)
            emit_marshaled_typed_struct(request_struct_name, typed_struct_fields(func.input, "in"), f, archive=archive)
            emit_marshaled_typed_struct(response_struct_name, typed_struct_fields(func.output, "out"), f, archive=archive)
        f.writeln("enum {")
        with f.indent():
            for func in service.functions:
                rpc_code = rpc_table["%s.%s" % (service.name, func.name)]
                f.writeln("%s = %s," % (func.name.upper(), hex(rpc_code)))
        f.writeln("};")
        f.writeln("// Registers RPC IDs with server using service index")
        f.writeln("// @unsafe - calls rrr::Server::reg_rpc / unreg (not borrow-checked)")
        f.writeln("int __reg_to__(rrr::Server& svr, size_t svc_index) {")
        with f.indent():
            f.writeln("int ret = 0;")
            for func in service.functions:
                reg_api = "reg_fast_rpc" if func.attr in ("fast", "prefix", "async") else "reg_rpc"
                f.writeln("if ((ret = svr.%s(%s, svc_index)) != 0) {" % (reg_api, func.name.upper()))
                with f.indent():
                    f.writeln("goto err;")
                f.writeln("}")
            f.writeln("return 0;")
        f.writeln("err:")
        with f.indent():
            for func in service.functions:
                f.writeln("svr.unreg(%s);" % func.name.upper())
            f.writeln("return ret;")
        f.writeln("}")
        f.writeln("// @safe - Dispatch for RPC requests")
        f.writeln("void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {")
        with f.indent():
            f.writeln("switch (rpc_id) {")
            for func in service.functions:
                if func.attr == "raw":
                    f.writeln("case %s: %s(std::move(req), weak_sconn); break;" % (func.name.upper(), func.name))
                else:
                    f.writeln("case %s: __%s__wrapper__(std::move(req), weak_sconn); break;" % (func.name.upper(), func.name))
            f.writeln("default: break;  // Unknown RPC ID, ignore")
            f.writeln("}")
        f.writeln("}")
        f.writeln("// typed service signatures")
        for func in service.functions:
            if func.attr == "raw":
                continue
            emit_typed_service_signature(service, func, f)
        f.writeln("// these RPC handler functions need to be implemented by user")
        f.writeln("// for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use")
        for func in service.functions:
            if service.abstract or func.abstract:
                postfix = " = 0"
            else:
                postfix = ""
            if func.attr == "raw":
                f.writeln("// @safe")
                f.writeln("virtual void %s(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn)%s;" % (func.name, postfix))
    f.writeln("private:")
    with f.indent():
        for func in service.functions:
            if func.attr == "raw":
                continue
            f.writeln("// @safe")
            f.writeln("void __%s__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {" % func.name)
            with f.indent():
                f.writeln("// @unsafe")
                f.writeln("{")
                with f.indent():
                    if func.attr == "defer":
                        request_struct_name = typed_request_struct_name(func)
                        response_struct_name = typed_response_struct_name(func)
                        input_fields = typed_struct_fields(func.input, "in")
                        output_fields = typed_struct_fields(func.output, "out")
                        f.writeln("%s __typed_req__;" % request_struct_name)
                        # decode incoming request
                        # bytes through BinaryReadArchive (matches the
                        # write-side archive emission landed in Phase 3d-3).
                        # MarshalSource bridges the legacy `req->m` Marshal
                        # to the archive's read API.
                        if len(input_fields) > 0:
                            f.writeln("rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));")
                            for _, field_name in input_fields:
                                f.writeln("rrr::Deserialize_::deserialize(__typed_req__.%s, __req_ar__);" % field_name)
                        f.writeln("auto __typed_resp__ = std::make_shared<%s>();" % response_struct_name)
                        f.writeln("auto __defer__ = rrr::DeferredReply::new_(")
                        with f.indent():
                            f.writeln("std::move(req),")
                            f.writeln("weak_sconn,")
                            f.writeln("[__typed_resp__](rrr::BinaryWriteArchive& m) {")
                            with f.indent():
                                for _, field_name in output_fields:
                                    f.writeln("rrr::Serialize_::serialize(__typed_resp__->%s, m);" % field_name)
                            f.writeln("},")
                            f.writeln("[]() {});")
                        f.writeln("this->%s(__typed_req__, *__typed_resp__, std::move(__defer__));" % func.name)
                    elif func.attr == "fiber":
                        request_struct_name = typed_request_struct_name(func)
                        output_fields = typed_struct_fields(func.output, "out")
                        input_fields = typed_struct_fields(func.input, "in")
                        f.writeln("%s __typed_req__;" % request_struct_name)
                        # see comment under
                        # `func.attr == "defer"`.
                        if len(input_fields) > 0:
                            f.writeln("rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));")
                            for _, field_name in input_fields:
                                f.writeln("rrr::Deserialize_::deserialize(__typed_req__.%s, __req_ar__);" % field_name)
                        f.writeln("auto __fiber_req__ = std::move(req);")
                        f.writeln("auto __fiber_weak_sconn__ = weak_sconn;")
                        f.writeln("auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {")
                        with f.indent():
                            f.writeln("auto __typed_result__ = this->%s(__typed_req__);" % func.name)
                            f.writeln("auto sconn_opt = __fiber_weak_sconn__.upgrade();")
                            f.writeln("if (sconn_opt.is_some()) {")
                            with f.indent():
                                f.writeln("auto sconn = sconn_opt.unwrap();")
                                f.writeln("if (__typed_result__.is_err()) {")
                                with f.indent():
                                    f.writeln("const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});")
                                f.writeln("} else {")
                                with f.indent():
                                    if len(output_fields) == 0:
                                        f.writeln("auto __typed_resp__ = __typed_result__.unwrap();")
                                        f.writeln("(void)__typed_resp__;")
                                        f.writeln("const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, rrr::ServerReplyFn{});")
                                    else:
                                        f.writeln("auto __typed_resp__ = __typed_result__.unwrap();")
                                        f.writeln("const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {")
                                        with f.indent():
                                            for _, field_name in output_fields:
                                                f.writeln("rrr::Serialize_::serialize(__typed_resp__.%s, m);" % field_name)
                                        f.writeln("});")
                                f.writeln("}")
                            f.writeln("}")
                        f.writeln("});")
                        f.writeln("(void)__fiber__;")
                    elif func.attr == "async":
                        request_struct_name = typed_request_struct_name(func)
                        output_fields = typed_struct_fields(func.output, "out")
                        input_fields = typed_struct_fields(func.input, "in")
                        f.writeln("%s __typed_req__;" % request_struct_name)
                        # see comment under
                        # `func.attr == "defer"`.
                        if len(input_fields) > 0:
                            f.writeln("rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));")
                            for _, field_name in input_fields:
                                f.writeln("rrr::Deserialize_::deserialize(__typed_req__.%s, __req_ar__);" % field_name)
                        f.writeln("auto __async_req__ = std::move(req);")
                        f.writeln("auto __async_weak_sconn__ = weak_sconn;")
                        f.writeln("auto __async_task__ = this->%s(__typed_req__);" % func.name)
                        f.writeln("rrr::Reactor::get_reactor()->spawn_stackless_task_with_result(std::move(__async_task__), [__async_req__ = std::move(__async_req__), __async_weak_sconn__](auto __typed_result__) mutable {")
                        with f.indent():
                            f.writeln("auto sconn_opt = __async_weak_sconn__.upgrade();")
                            f.writeln("if (sconn_opt.is_some()) {")
                            with f.indent():
                                f.writeln("auto sconn = sconn_opt.unwrap();")
                                f.writeln("if (__typed_result__.is_err()) {")
                                with f.indent():
                                    f.writeln("const_cast<rrr::ServerConnection&>(*sconn).reply(*__async_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});")
                                f.writeln("} else {")
                                with f.indent():
                                    if len(output_fields) == 0:
                                        f.writeln("auto __typed_resp__ = __typed_result__.unwrap();")
                                        f.writeln("(void)__typed_resp__;")
                                        f.writeln("const_cast<rrr::ServerConnection&>(*sconn).reply(*__async_req__, 0, rrr::ServerReplyFn{});")
                                    else:
                                        f.writeln("auto __typed_resp__ = __typed_result__.unwrap();")
                                        f.writeln("const_cast<rrr::ServerConnection&>(*sconn).reply(*__async_req__, 0, [&](rrr::BinaryWriteArchive& m) {")
                                        with f.indent():
                                            for _, field_name in output_fields:
                                                f.writeln("rrr::Serialize_::serialize(__typed_resp__.%s, m);" % field_name)
                                        f.writeln("});")
                                f.writeln("}")
                            f.writeln("}")
                        f.writeln("});")
                    else: # normal and fast rpc
                        request_struct_name = typed_request_struct_name(func)
                        output_fields = typed_struct_fields(func.output, "out")
                        input_fields = typed_struct_fields(func.input, "in")
                        f.writeln("%s __typed_req__;" % request_struct_name)
                        # see comment under
                        # `func.attr == "defer"`.
                        if len(input_fields) > 0:
                            f.writeln("rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));")
                            for _, field_name in input_fields:
                                f.writeln("rrr::Deserialize_::deserialize(__typed_req__.%s, __req_ar__);" % field_name)
                        f.writeln("auto __typed_result__ = this->%s(__typed_req__);" % func.name)
                        f.writeln("auto sconn_opt = weak_sconn.upgrade();")
                        f.writeln("if (sconn_opt.is_some()) {")
                        with f.indent():
                            f.writeln("auto sconn = sconn_opt.unwrap();")
                            f.writeln("if (__typed_result__.is_err()) {")
                            with f.indent():
                                f.writeln("const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});")
                            f.writeln("} else {")
                            with f.indent():
                                if len(output_fields) == 0:
                                    f.writeln("auto __typed_resp__ = __typed_result__.unwrap();")
                                    f.writeln("(void)__typed_resp__;")
                                    f.writeln("const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, rrr::ServerReplyFn{});")
                                else:
                                    f.writeln("auto __typed_resp__ = __typed_result__.unwrap();")
                                    f.writeln("const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::BinaryWriteArchive& m) {")
                                    with f.indent():
                                        for _, field_name in output_fields:
                                            f.writeln("rrr::Serialize_::serialize(__typed_resp__.%s, m);" % field_name)
                                    f.writeln("});")
                            f.writeln("}")
                        f.writeln("}")
                        f.writeln("// req automatically cleaned up by rusty::Box")
                f.writeln("}")
            f.writeln("}")
    f.writeln("};")
    f.writeln()
    f.writeln("class %sProxy {" % service.name)
    f.writeln("protected:")
    with f.indent():
        f.writeln("rrr::Client* __cl__;")
    f.writeln("public:")
    with f.indent():
        f.writeln("%sProxy(rrr::Client* cl): __cl__(cl) { }" % service.name)
        f.writeln("// Alias typed request/response structs from the sibling Service class.")
        for func in service.functions:
            f.writeln("using %s = %sService::%s;" % (
                typed_request_struct_name(func),
                service.name,
                typed_request_struct_name(func),
            ))
            f.writeln("using %s = %sService::%s;" % (
                typed_response_struct_name(func),
                service.name,
                typed_response_struct_name(func),
            ))
        for func in service.functions:
            if func.attr != "raw":
                typed_async_call_params = []
                for _, field_name in typed_struct_fields(func.input, "in"):
                    typed_async_call_params += "req.%s" % field_name,
                emit_typed_proxy_future_wrapper(func, f)
                emit_typed_proxy_async_signature(service, func, typed_async_call_params, f)
                emit_typed_proxy_await_signature(func, f)
                emit_typed_proxy_sync_signature(func, f)
            else:
                async_func_params = []
                async_call_params = []
                sync_func_params = []
                sync_out_params = []
                in_counter = 0
                out_counter = 0
                for in_arg in func.input:
                    if in_arg.name != None:
                        async_func_params += "const %s& %s" % (in_arg.type, in_arg.name),
                        async_call_params += in_arg.name,
                        sync_func_params += "const %s& %s" % (in_arg.type, in_arg.name),
                    else:
                        async_func_params += "const %s& in_%d" % (in_arg.type, in_counter),
                        async_call_params += "in_%d" % in_counter,
                        sync_func_params += "const %s& in_%d" % (in_arg.type, in_counter),
                    in_counter += 1
                for out_arg in func.output:
                    if out_arg.name != None:
                        sync_func_params += "%s* %s" % (out_arg.type, out_arg.name),
                        sync_out_params += out_arg.name,
                    else:
                        sync_func_params += "%s* out_%d" % (out_arg.type, out_counter),
                        sync_out_params += "out_%d" % out_counter,
                    out_counter += 1
                f.writeln("rrr::FutureResult async_%s(%sconst rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {" % (func.name, ", ".join(async_func_params + [""])))
                with f.indent():
                    emit_proxy_request_call(service, func, async_call_params, f)
                f.writeln("}")
                f.writeln("rrr::i32 %s(%s) {" % (func.name, ", ".join(sync_func_params)))
                with f.indent():
                    f.writeln("auto __fu_result__ = this->async_%s(%s);" % (func.name, ", ".join(async_call_params)))
                    f.writeln("if (__fu_result__.is_err()) {")
                    with f.indent():
                        f.writeln("return __fu_result__.unwrap_err();  // Return error code")
                    f.writeln("}")
                    f.writeln("auto __fu__ = __fu_result__.unwrap();")
                    f.writeln("rrr::i32 __ret__ = __fu__->get_error_code();")
                    if len(sync_out_params) > 0:
                        f.writeln("if (__ret__ == 0) {")
                        with f.indent():
                            # decode reply bytes
                            # through BinaryReadArchive — see the matching
                            # comment in `emit_typed_proxy_future_wrapper`.
                            f.writeln("auto __reply_guard__ = __fu__->get_reply();")
                            f.writeln("rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));")
                            for param in sync_out_params:
                                f.writeln("rrr::Deserialize_::deserialize(*%s, __reply_ar__);" % param)
                        f.writeln("}")
                    f.writeln("// Arc auto-released")
                    f.writeln("return __ret__;")
                f.writeln("}")
    f.writeln("};")
    f.writeln()


def emit_rpc_source_cpp(rpc_source, rpc_table, fpath, cpp_header, cpp_footer, archive=False):
    with open(fpath, "w") as f:
        f = SourceFile(f)
        f.writeln("#pragma once")
        f.writeln()
#        f.writeln('#include "rpc/server.h"')
        f.writeln('#include "rrr/rrr.hpp"')
        f.writeln('#include <rusty/async.hpp>')
        f.writeln('#include <rusty/arc.hpp>')
        f.writeln('#include <rusty/box.hpp>')
        f.writeln('#include <rusty/result.hpp>')
        f.writeln()
        f.writeln("#include <errno.h>")
        f.writeln("#include <memory>")
        f.writeln()
        f.write(cpp_header)
        f.writeln()

        if rpc_source.namespace != None:
            f.writeln(" ".join(map(lambda x:"namespace %s {" % x, rpc_source.namespace)))
            f.writeln()

        for struct in rpc_source.structs:
            emit_struct(struct, f, archive=archive)

        for service in rpc_source.services:
            emit_service_and_proxy(service, f, rpc_table, archive=archive)

        if rpc_source.namespace != None:
            f.writeln(" ".join(["}"] * len(rpc_source.namespace)) + " // namespace " + "::".join(rpc_source.namespace))
            f.writeln()

        f.writeln()
        f.write(cpp_footer)
        f.writeln()
