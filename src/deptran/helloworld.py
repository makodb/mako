import os
from simplerpc.marshal import Marshal
from simplerpc.future import Future

class HelloworldClientService(object):
    TXN_READ = 0x2e6830d2

    __input_type_info__ = {
        'txn_read': ['std::vector<rrr::i64>'],
    }

    __output_type_info__ = {
        'txn_read': ['rrr::i32'],
    }

    def __bind_helper__(self, func):
        def f(*args):
            return getattr(self, func.__name__)(*args)
        return f

    def __reg_to__(self, server):
        server.__reg_func__(HelloworldClientService.TXN_READ, self.__bind_helper__(self.txn_read), ['std::vector<rrr::i64>'], ['rrr::i32'])

    def txn_read(__self__, _req):
        raise NotImplementedError('subclass HelloworldClientService and implement your own txn_read function')

class HelloworldClientProxy(object):
    def __init__(self, clnt):
        self.__clnt__ = clnt

    def async_txn_read(__self__, _req):
        return __self__.__clnt__.async_call(HelloworldClientService.TXN_READ, [_req], HelloworldClientService.__input_type_info__['txn_read'], HelloworldClientService.__output_type_info__['txn_read'])

    def sync_txn_read(__self__, _req):
        __result__ = __self__.__clnt__.sync_call(HelloworldClientService.TXN_READ, [_req], HelloworldClientService.__input_type_info__['txn_read'], HelloworldClientService.__output_type_info__['txn_read'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

