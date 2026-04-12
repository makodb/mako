from simplerpcgen.misc import SourceFile

def emit_struct(struct, f):
    f.writeln("struct %s {" % struct.name)
    with f.indent():
        for field in struct.fields:
            f.writeln("%s %s;" % (field.type, field.name))
    f.writeln("};")
    f.writeln()
    f.writeln("inline rrr::Marshal& operator <<(rrr::Marshal& m, const %s& o) {" % struct.name)
    with f.indent():
        for field in struct.fields:
            f.writeln("m << o.%s;" % field.name)
        f.writeln("return m;")
    f.writeln("}")
    f.writeln()
    f.writeln("inline rrr::Marshal& operator >>(rrr::Marshal& m, %s& o) {" % struct.name)
    with f.indent():
        for field in struct.fields:
            f.writeln("m >> o.%s;" % field.name)
        f.writeln("return m;")
    f.writeln("}")
    f.writeln()

def typed_struct_name(func_name, suffix):
    return "%s%s" % (func_name, suffix)

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

def emit_marshaled_typed_struct(struct_name, fields, f):
    f.writeln("struct %s {" % struct_name)
    with f.indent():
        for field_type, field_name in fields:
            f.writeln("%s %s;" % (field_type, field_name))
    f.writeln("};")
    f.writeln("friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const %s& o) {" % struct_name)
    with f.indent():
        for _, field_name in fields:
            f.writeln("m << o.%s;" % field_name)
        f.writeln("return m;")
    f.writeln("}")
    f.writeln("friend inline rrr::Marshal& operator >>(rrr::Marshal& m, %s& o) {" % struct_name)
    with f.indent():
        for _, field_name in fields:
            f.writeln("m >> o.%s;" % field_name)
        f.writeln("return m;")
    f.writeln("}")
    f.writeln()

def emit_typed_service_signature(func, f):
    request_struct_name = typed_request_struct_name(func)
    response_struct_name = typed_response_struct_name(func)
    result_type = typed_result_type(func)
    input_fields = typed_struct_fields(func.input, "in")
    output_fields = typed_struct_fields(func.output, "out")

    f.writeln("virtual %s %s(const %s& req) {" % (result_type, func.name, request_struct_name))
    with f.indent():
        if func.attr == "defer":
            f.writeln("(void)req;")
            f.writeln("return %s::Err(ENOTSUP);" % result_type)
        else:
            f.writeln("%s __typed_resp__;" % response_struct_name)
            invoke_with = []
            for _, field_name in input_fields:
                invoke_with += "req.%s" % field_name,
            for _, field_name in output_fields:
                invoke_with += "&__typed_resp__.%s" % field_name,
            f.writeln("this->%s(%s);" % (func.name, ", ".join(invoke_with)))
            if len(input_fields) == 0:
                f.writeln("(void)req;")
            f.writeln("return %s::Ok(__typed_resp__);" % result_type)
    f.writeln("}")

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
    if len(marshal_args) > 0:
        f.writeln("return __cl__->request(%sService::%s, __fu_attr__, [&](rrr::Marshal& __m__) {" % (service.name, func.name.upper()))
        with f.indent():
            for arg in marshal_args:
                f.writeln("__m__ << %s;" % arg)
        f.writeln("});")
    else:
        f.writeln("return __cl__->request(%sService::%s, __fu_attr__);" % (service.name, func.name.upper()))

def emit_typed_request_from_args(func, input_args, request_var_name, f):
    request_struct_name = typed_request_struct_name(func)
    input_fields = typed_struct_fields(func.input, "in")

    f.writeln("%s %s;" % (request_struct_name, request_var_name))
    for idx, (_, field_name) in enumerate(input_fields):
        f.writeln("%s.%s = %s;" % (request_var_name, field_name, input_args[idx]))

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
            for _, field_name in output_fields:
                f.writeln("__fu__->get_reply() >> __typed_resp__.%s;" % field_name)
            f.writeln("return %s::Ok(__typed_resp__);" % result_type)
        f.writeln("}")
    f.writeln("};")

def emit_typed_proxy_async_signature(service, func, typed_async_call_params, f):
    request_struct_name = typed_request_struct_name(func)
    wrapper_name = typed_proxy_future_wrapper_name(func)
    result_type = typed_proxy_future_result_type(func)

    f.writeln("%s async_%s(const %s& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {" % (result_type, func.name, request_struct_name))
    with f.indent():
        if len(typed_async_call_params) > 0:
            f.writeln("auto __fu_result__ = __cl__->request(%sService::%s, __fu_attr__, [&](rrr::Marshal& __m__) {" % (service.name, func.name.upper()))
            with f.indent():
                for param in typed_async_call_params:
                    f.writeln("__m__ << %s;" % param)
            f.writeln("});")
        else:
            f.writeln("auto __fu_result__ = __cl__->request(%sService::%s, __fu_attr__);" % (service.name, func.name.upper()))
        f.writeln("if (__fu_result__.is_err()) {")
        with f.indent():
            f.writeln("return %s::Err(__fu_result__.unwrap_err());" % result_type)
        f.writeln("}")
        if len(typed_async_call_params) == 0:
            f.writeln("(void)req;")
        f.writeln("return %s::Ok(%s(__fu_result__.unwrap()));" % (result_type, wrapper_name))
    f.writeln("}")

def emit_service_and_proxy(service, f, rpc_table):
    f.writeln("class %sService: public rrr::Service {" % service.name)
    f.writeln("public:")
    with f.indent():
        f.writeln("// Typed request/response scaffolding generated from RPC signature lists.")
        for func in service.functions:
            request_struct_name = typed_request_struct_name(func)
            response_struct_name = typed_response_struct_name(func)
            emit_marshaled_typed_struct(request_struct_name, typed_struct_fields(func.input, "in"), f)
            emit_marshaled_typed_struct(response_struct_name, typed_struct_fields(func.output, "out"), f)
        f.writeln("enum {")
        with f.indent():
            for func in service.functions:
                rpc_code = rpc_table["%s.%s" % (service.name, func.name)]
                f.writeln("%s = %s," % (func.name.upper(), hex(rpc_code)))
        f.writeln("};")
        f.writeln("// Registers RPC IDs with server using service index")
        f.writeln("// @safe")
        f.writeln("int __reg_to__(rrr::Server& svr, size_t svc_index) override {")
        with f.indent():
            f.writeln("int ret = 0;")
            for func in service.functions:
                f.writeln("if ((ret = svr.reg_rpc(%s, svc_index)) != 0) {" % func.name.upper())
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
        f.writeln("// @safe - Virtual dispatch for RPC requests")
        f.writeln("void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) override {")
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
        f.writeln("// typed service signatures for request/response migration")
        f.writeln("// compatibility mode keeps pointer-style handlers as the runtime dispatch path")
        for func in service.functions:
            if func.attr == "raw":
                continue
            emit_typed_service_signature(func, f)
        f.writeln("// these RPC handler functions need to be implemented by user")
        f.writeln("// for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use")
        for func in service.functions:
            if service.abstract or func.abstract:
                postfix = " = 0"
            else:
                postfix = ""
            if func.attr == "raw":
                f.writeln("virtual void %s(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn)%s;" % (func.name, postfix))
            else:
                func_args = []
                for in_arg in func.input:
                    if in_arg.name != None:
                        func_args += "const %s& %s" % (in_arg.type, in_arg.name),
                    else:
                        func_args += "const %s&" % in_arg.type,
                for out_arg in func.output:
                    if out_arg.name != None:
                        func_args += "%s* %s" % (out_arg.type, out_arg.name),
                    else:
                        func_args += "%s*" % out_arg.type,
                if func.attr == "defer":
                    func_args += "rrr::DeferredReply defer",
                f.writeln("virtual void %s(%s)%s;" % (func.name, ", ".join(func_args), postfix))
    f.writeln("private:")
    with f.indent():
        for func in service.functions:
            if func.attr == "raw":
                continue
            f.writeln("void __%s__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {" % func.name)
            with f.indent():
                if func.attr == "defer":
                    invoke_with = []
                    in_counter = 0
                    out_counter = 0
                    for in_arg in func.input:
                        f.writeln("%s* in_%d = new %s;" % (in_arg.type, in_counter, in_arg.type))
                        f.writeln("req->m >> *in_%d;" % in_counter)
                        invoke_with += "*in_%d" % in_counter,
                        in_counter += 1
                    for out_arg in func.output:
                        f.writeln("%s* out_%d = new %s;" % (out_arg.type, out_counter, out_arg.type))
                        invoke_with += "out_%d" % out_counter,
                        out_counter += 1
                    f.writeln("auto __marshal_reply__ = [=](rrr::Marshal& m) {");
                    with f.indent():
                        out_counter = 0
                        for out_arg in func.output:
                            f.writeln("m << *out_%d;" % out_counter)
                            out_counter += 1
                    f.writeln("};");
                    f.writeln("auto __cleanup__ = [=] {");
                    with f.indent():
                        in_counter = 0
                        out_counter = 0
                        for in_arg in func.input:
                            f.writeln("delete in_%d;" % in_counter)
                            in_counter += 1
                        for out_arg in func.output:
                            f.writeln("delete out_%d;" % out_counter)
                            out_counter += 1
                    f.writeln("};");
                    f.writeln("rrr::DeferredReply __defer__(std::move(req), weak_sconn, __marshal_reply__, __cleanup__);")
                    invoke_with += "std::move(__defer__)",
                    f.writeln("this->%s(%s);" % (func.name, ", ".join(invoke_with)))
                else: # normal and fast rpc
                    # Don't use lambda - execute directly for all methods
                    invoke_with = []
                    in_counter = 0
                    out_counter = 0
                    for in_arg in func.input:
                        f.writeln("%s in_%d;" % (in_arg.type, in_counter))
                        f.writeln("req->m >> in_%d;" % in_counter)
                        invoke_with += "in_%d" % in_counter,
                        in_counter += 1
                    for out_arg in func.output:
                        f.writeln("%s out_%d;" % (out_arg.type, out_counter))
                        invoke_with += "&out_%d" % out_counter,
                        out_counter += 1
                    f.writeln("this->%s(%s);" % (func.name, ", ".join(invoke_with)))
                    f.writeln("auto sconn_opt = weak_sconn.upgrade();")
                    f.writeln("if (sconn_opt.is_some()) {")
                    with f.indent():
                        f.writeln("auto sconn = sconn_opt.unwrap();")
                        if out_counter == 0:
                            f.writeln("const_cast<rrr::ServerConnection&>(*sconn).reply(*req);")
                        else:
                            f.writeln("const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::Marshal& m) {")
                            with f.indent():
                                for i in range(out_counter):
                                    f.writeln("m << out_%d;" % i)
                            f.writeln("});")
                    f.writeln("}")
                    f.writeln("// req automatically cleaned up by rusty::Box")
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
        for func in service.functions:
            async_func_params = []
            async_call_params = []
            sync_func_params = []
            sync_out_params = []
            typed_async_call_params = []
            typed_output_fields = typed_struct_fields(func.output, "out")
            in_counter = 0
            out_counter = 0
            for in_arg in func.input:
                if in_arg.name != None:
                    async_func_params += "const %s& %s" % (in_arg.type, in_arg.name),
                    async_call_params += in_arg.name,
                    sync_func_params += "const %s& %s" % (in_arg.type, in_arg.name),
                    typed_async_call_params += "req.%s" % in_arg.name,
                else:
                    async_func_params += "const %s& in_%d" % (in_arg.type, in_counter),
                    async_call_params += "in_%d" % in_counter,
                    sync_func_params += "const %s& in_%d" % (in_arg.type, in_counter),
                    typed_async_call_params += "req.in_%d" % in_counter,
                in_counter += 1
            for out_arg in func.output:
                if out_arg.name != None:
                    sync_func_params += "%s* %s" % (out_arg.type, out_arg.name),
                    sync_out_params += out_arg.name,
                else:
                    sync_func_params += "%s* out_%d" % (out_arg.type, out_counter),
                    sync_out_params += "out_%d" % out_counter,
                out_counter += 1
            if func.attr != "raw":
                emit_typed_proxy_future_wrapper(func, f)
            f.writeln("rrr::FutureResult async_%s(%sconst rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {" % (func.name, ", ".join(async_func_params + [""])))
            with f.indent():
                if func.attr != "raw":
                    emit_typed_request_from_args(func, async_call_params, "__typed_req__", f)
                    f.writeln("auto __typed_fu_result__ = this->async_%s(__typed_req__, __fu_attr__);" % func.name)
                    f.writeln("if (__typed_fu_result__.is_err()) {")
                    with f.indent():
                        f.writeln("return rrr::FutureResult::Err(__typed_fu_result__.unwrap_err());")
                    f.writeln("}")
                    f.writeln("return rrr::FutureResult::Ok(__typed_fu_result__.unwrap().raw_future());")
                else:
                    emit_proxy_request_call(service, func, async_call_params, f)
            f.writeln("}")
            if func.attr != "raw":
                emit_typed_proxy_async_signature(service, func, typed_async_call_params, f)
            f.writeln("rrr::i32 %s(%s) {" % (func.name, ", ".join(sync_func_params)))
            with f.indent():
                if func.attr != "raw":
                    emit_typed_request_from_args(func, async_call_params, "__typed_req__", f)
                    f.writeln("auto __typed_result__ = this->%s(__typed_req__);" % func.name)
                    f.writeln("if (__typed_result__.is_err()) {")
                    with f.indent():
                        f.writeln("return __typed_result__.unwrap_err();")
                    f.writeln("}")
                    f.writeln("auto __typed_resp__ = __typed_result__.unwrap();")
                    if len(sync_out_params) > 0:
                        for idx, param in enumerate(sync_out_params):
                            f.writeln("*%s = __typed_resp__.%s;" % (param, typed_output_fields[idx][1]))
                    else:
                        f.writeln("(void)__typed_resp__;")
                    f.writeln("return 0;")
                else:
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
                            for param in sync_out_params:
                                f.writeln("__fu__->get_reply() >> *%s;" % param)
                        f.writeln("}")
                    f.writeln("// Arc auto-released")
                    f.writeln("return __ret__;")
            f.writeln("}")
            if func.attr != "raw":
                emit_typed_proxy_sync_signature(func, f)
    f.writeln("};")
    f.writeln()


def emit_rpc_source_cpp(rpc_source, rpc_table, fpath, cpp_header, cpp_footer):
    with open(fpath, "w") as f:
        f = SourceFile(f)
        f.writeln("#pragma once")
        f.writeln()
#        f.writeln('#include "rpc/server.h"')
        f.writeln('#include "rrr.hpp"')
        f.writeln()
        f.writeln("#include <errno.h>")
        f.writeln()
        f.write(cpp_header)
        f.writeln()

        if rpc_source.namespace != None:
            f.writeln(" ".join(map(lambda x:"namespace %s {" % x, rpc_source.namespace)))
            f.writeln()

        for struct in rpc_source.structs:
            emit_struct(struct, f)

        for service in rpc_source.services:
            emit_service_and_proxy(service, f, rpc_table)

        if rpc_source.namespace != None:
            f.writeln(" ".join(["}"] * len(rpc_source.namespace)) + " // namespace " + "::".join(rpc_source.namespace))
            f.writeln()

        f.writeln()
        f.write(cpp_footer)
        f.writeln()
