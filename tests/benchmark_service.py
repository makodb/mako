import os
from simplerpc.marshal import Marshal
from simplerpc.future import Future

point3 = Marshal.reg_type('point3', [('x', 'double'), ('y', 'double'), ('z', 'double')])

class BenchmarkService(object):
    FAST_PRIME = 0x4f4daa5a
    FAST_DOT_PROD = 0x36ff5226
    FAST_ADD = 0x3a24232d
    FAST_NOP = 0x4b921bd9
    FAST_VEC = 0x23928fcb
    PRIME = 0x4e81b3fc
    DOT_PROD = 0x1f7d12f4
    ADD = 0x1e8ff45b
    NOP = 0x327203ee
    ASYNC_NOP = 0x22654490
    SLEEP = 0x22cb72f2
    DEFERRED_ECHO = 0x412ef56f

    __input_type_info__ = {
        'fast_prime': ['i32'],
        'fast_dot_prod': ['point3','point3'],
        'fast_add': ['v32','v32'],
        'fast_nop': ['std::string'],
        'fast_vec': ['i32'],
        'prime': ['i32'],
        'dot_prod': ['point3','point3'],
        'add': ['v32','v32'],
        'nop': ['std::string'],
        'async_nop': ['std::string'],
        'sleep': ['double'],
        'deferred_echo': ['i32'],
    }

    __output_type_info__ = {
        'fast_prime': ['i8'],
        'fast_dot_prod': ['double'],
        'fast_add': ['v32'],
        'fast_nop': [],
        'fast_vec': ['std::vector<i64>'],
        'prime': ['i8'],
        'dot_prod': ['double'],
        'add': ['v32'],
        'nop': [],
        'async_nop': [],
        'sleep': [],
        'deferred_echo': ['i32'],
    }

    def __bind_helper__(self, func):
        def f(*args):
            return getattr(self, func.__name__)(*args)
        return f

    def __reg_to__(self, server):
        server.__reg_func__(BenchmarkService.FAST_PRIME, self.__bind_helper__(self.fast_prime), ['i32'], ['i8'])
        server.__reg_func__(BenchmarkService.FAST_DOT_PROD, self.__bind_helper__(self.fast_dot_prod), ['point3','point3'], ['double'])
        server.__reg_func__(BenchmarkService.FAST_ADD, self.__bind_helper__(self.fast_add), ['v32','v32'], ['v32'])
        server.__reg_func__(BenchmarkService.FAST_NOP, self.__bind_helper__(self.fast_nop), ['std::string'], [])
        server.__reg_func__(BenchmarkService.FAST_VEC, self.__bind_helper__(self.fast_vec), ['i32'], ['std::vector<i64>'])
        server.__reg_func__(BenchmarkService.PRIME, self.__bind_helper__(self.prime), ['i32'], ['i8'])
        server.__reg_func__(BenchmarkService.DOT_PROD, self.__bind_helper__(self.dot_prod), ['point3','point3'], ['double'])
        server.__reg_func__(BenchmarkService.ADD, self.__bind_helper__(self.add), ['v32','v32'], ['v32'])
        server.__reg_func__(BenchmarkService.NOP, self.__bind_helper__(self.nop), ['std::string'], [])
        server.__reg_func__(BenchmarkService.ASYNC_NOP, self.__bind_helper__(self.async_nop), ['std::string'], [])
        server.__reg_func__(BenchmarkService.SLEEP, self.__bind_helper__(self.sleep), ['double'], [])
        server.__reg_func__(BenchmarkService.DEFERRED_ECHO, self.__bind_helper__(self.deferred_echo), ['i32'], ['i32'])

    def fast_prime(__self__, n):
        raise NotImplementedError('subclass BenchmarkService and implement your own fast_prime function')

    def fast_dot_prod(__self__, p1, p2):
        raise NotImplementedError('subclass BenchmarkService and implement your own fast_dot_prod function')

    def fast_add(__self__, a, b):
        raise NotImplementedError('subclass BenchmarkService and implement your own fast_add function')

    def fast_nop(__self__, in0):
        raise NotImplementedError('subclass BenchmarkService and implement your own fast_nop function')

    def fast_vec(__self__, n):
        raise NotImplementedError('subclass BenchmarkService and implement your own fast_vec function')

    def prime(__self__, n):
        raise NotImplementedError('subclass BenchmarkService and implement your own prime function')

    def dot_prod(__self__, p1, p2):
        raise NotImplementedError('subclass BenchmarkService and implement your own dot_prod function')

    def add(__self__, a, b):
        raise NotImplementedError('subclass BenchmarkService and implement your own add function')

    def nop(__self__, in0):
        raise NotImplementedError('subclass BenchmarkService and implement your own nop function')

    def async_nop(__self__, in0):
        raise NotImplementedError('subclass BenchmarkService and implement your own async_nop function')

    def sleep(__self__, sec):
        raise NotImplementedError('subclass BenchmarkService and implement your own sleep function')

    def deferred_echo(__self__, val):
        raise NotImplementedError('subclass BenchmarkService and implement your own deferred_echo function')

class BenchmarkProxy(object):
    def __init__(self, clnt):
        self.__clnt__ = clnt

    def async_fast_prime(__self__, n):
        return __self__.__clnt__.async_call(BenchmarkService.FAST_PRIME, [n], BenchmarkService.__input_type_info__['fast_prime'], BenchmarkService.__output_type_info__['fast_prime'])

    def async_fast_dot_prod(__self__, p1, p2):
        return __self__.__clnt__.async_call(BenchmarkService.FAST_DOT_PROD, [p1, p2], BenchmarkService.__input_type_info__['fast_dot_prod'], BenchmarkService.__output_type_info__['fast_dot_prod'])

    def async_fast_add(__self__, a, b):
        return __self__.__clnt__.async_call(BenchmarkService.FAST_ADD, [a, b], BenchmarkService.__input_type_info__['fast_add'], BenchmarkService.__output_type_info__['fast_add'])

    def async_fast_nop(__self__, in0):
        return __self__.__clnt__.async_call(BenchmarkService.FAST_NOP, [in0], BenchmarkService.__input_type_info__['fast_nop'], BenchmarkService.__output_type_info__['fast_nop'])

    def async_fast_vec(__self__, n):
        return __self__.__clnt__.async_call(BenchmarkService.FAST_VEC, [n], BenchmarkService.__input_type_info__['fast_vec'], BenchmarkService.__output_type_info__['fast_vec'])

    def async_prime(__self__, n):
        return __self__.__clnt__.async_call(BenchmarkService.PRIME, [n], BenchmarkService.__input_type_info__['prime'], BenchmarkService.__output_type_info__['prime'])

    def async_dot_prod(__self__, p1, p2):
        return __self__.__clnt__.async_call(BenchmarkService.DOT_PROD, [p1, p2], BenchmarkService.__input_type_info__['dot_prod'], BenchmarkService.__output_type_info__['dot_prod'])

    def async_add(__self__, a, b):
        return __self__.__clnt__.async_call(BenchmarkService.ADD, [a, b], BenchmarkService.__input_type_info__['add'], BenchmarkService.__output_type_info__['add'])

    def async_nop(__self__, in0):
        return __self__.__clnt__.async_call(BenchmarkService.NOP, [in0], BenchmarkService.__input_type_info__['nop'], BenchmarkService.__output_type_info__['nop'])

    def async_async_nop(__self__, in0):
        return __self__.__clnt__.async_call(BenchmarkService.ASYNC_NOP, [in0], BenchmarkService.__input_type_info__['async_nop'], BenchmarkService.__output_type_info__['async_nop'])

    def async_sleep(__self__, sec):
        return __self__.__clnt__.async_call(BenchmarkService.SLEEP, [sec], BenchmarkService.__input_type_info__['sleep'], BenchmarkService.__output_type_info__['sleep'])

    def async_deferred_echo(__self__, val):
        return __self__.__clnt__.async_call(BenchmarkService.DEFERRED_ECHO, [val], BenchmarkService.__input_type_info__['deferred_echo'], BenchmarkService.__output_type_info__['deferred_echo'])

    def sync_fast_prime(__self__, n):
        __result__ = __self__.__clnt__.sync_call(BenchmarkService.FAST_PRIME, [n], BenchmarkService.__input_type_info__['fast_prime'], BenchmarkService.__output_type_info__['fast_prime'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_fast_dot_prod(__self__, p1, p2):
        __result__ = __self__.__clnt__.sync_call(BenchmarkService.FAST_DOT_PROD, [p1, p2], BenchmarkService.__input_type_info__['fast_dot_prod'], BenchmarkService.__output_type_info__['fast_dot_prod'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_fast_add(__self__, a, b):
        __result__ = __self__.__clnt__.sync_call(BenchmarkService.FAST_ADD, [a, b], BenchmarkService.__input_type_info__['fast_add'], BenchmarkService.__output_type_info__['fast_add'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_fast_nop(__self__, in0):
        __result__ = __self__.__clnt__.sync_call(BenchmarkService.FAST_NOP, [in0], BenchmarkService.__input_type_info__['fast_nop'], BenchmarkService.__output_type_info__['fast_nop'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_fast_vec(__self__, n):
        __result__ = __self__.__clnt__.sync_call(BenchmarkService.FAST_VEC, [n], BenchmarkService.__input_type_info__['fast_vec'], BenchmarkService.__output_type_info__['fast_vec'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_prime(__self__, n):
        __result__ = __self__.__clnt__.sync_call(BenchmarkService.PRIME, [n], BenchmarkService.__input_type_info__['prime'], BenchmarkService.__output_type_info__['prime'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_dot_prod(__self__, p1, p2):
        __result__ = __self__.__clnt__.sync_call(BenchmarkService.DOT_PROD, [p1, p2], BenchmarkService.__input_type_info__['dot_prod'], BenchmarkService.__output_type_info__['dot_prod'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_add(__self__, a, b):
        __result__ = __self__.__clnt__.sync_call(BenchmarkService.ADD, [a, b], BenchmarkService.__input_type_info__['add'], BenchmarkService.__output_type_info__['add'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_nop(__self__, in0):
        __result__ = __self__.__clnt__.sync_call(BenchmarkService.NOP, [in0], BenchmarkService.__input_type_info__['nop'], BenchmarkService.__output_type_info__['nop'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_async_nop(__self__, in0):
        __result__ = __self__.__clnt__.sync_call(BenchmarkService.ASYNC_NOP, [in0], BenchmarkService.__input_type_info__['async_nop'], BenchmarkService.__output_type_info__['async_nop'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_sleep(__self__, sec):
        __result__ = __self__.__clnt__.sync_call(BenchmarkService.SLEEP, [sec], BenchmarkService.__input_type_info__['sleep'], BenchmarkService.__output_type_info__['sleep'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_deferred_echo(__self__, val):
        __result__ = __self__.__clnt__.sync_call(BenchmarkService.DEFERRED_ECHO, [val], BenchmarkService.__input_type_info__['deferred_echo'], BenchmarkService.__output_type_info__['deferred_echo'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

