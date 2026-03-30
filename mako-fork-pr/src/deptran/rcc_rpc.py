import os
from simplerpc.marshal import Marshal
from simplerpc.future import Future

ValueTimesPair = Marshal.reg_type('ValueTimesPair', [('value', 'rrr::i64'), ('times', 'rrr::i64')])

DepId = Marshal.reg_type('DepId', [('str', 'std::string'), ('id', 'rrr::i64')])

TxnInfoRes = Marshal.reg_type('TxnInfoRes', [('start_txn', 'rrr::i32'), ('total_txn', 'rrr::i32'), ('total_try', 'rrr::i32'), ('commit_txn', 'rrr::i32'), ('num_exhausted', 'rrr::i32'), ('this_latency', 'std::vector<double>'), ('last_latency', 'std::vector<double>'), ('attempt_latency', 'std::vector<double>'), ('interval_latency', 'std::vector<double>'), ('all_interval_latency', 'std::vector<double>'), ('num_try', 'std::vector<rrr::i32>')])

ServerResponse = Marshal.reg_type('ServerResponse', [('statistics', 'std::map<std::string, ValueTimesPair>'), ('cpu_util', 'double'), ('r_cnt_sum', 'rrr::i64'), ('r_cnt_num', 'rrr::i64'), ('r_sz_sum', 'rrr::i64'), ('r_sz_num', 'rrr::i64')])

ClientResponse = Marshal.reg_type('ClientResponse', [('txn_info', 'std::map<rrr::i32, TxnInfoRes>'), ('run_sec', 'rrr::i64'), ('run_nsec', 'rrr::i64'), ('period_sec', 'rrr::i64'), ('period_nsec', 'rrr::i64'), ('is_finish', 'rrr::i32'), ('n_asking', 'rrr::i64')])

Profiling = Marshal.reg_type('Profiling', [('cpu_util', 'double'), ('tx_util', 'double'), ('rx_util', 'double'), ('mem_util', 'double')])

TxDispatchRequest = Marshal.reg_type('TxDispatchRequest', [('id', 'rrr::i32'), ('tx_type', 'rrr::i32'), ('input', 'std::vector<Value>')])

TxnDispatchResponse = Marshal.reg_type('TxnDispatchResponse', [])

class MultiPaxosService(object):
    FORWARD = 0x4bd7bb59
    PREPARE = 0x2e8e35b3
    ACCEPT = 0x4e66e836
    DECIDE = 0x2cad4dd1
    HEARTBEAT = 0x1392f62d
    FORWARDTOLEARNERSERVER = 0x66cd968f
    BULKPREPARE = 0x1ef8b4e5
    BULKACCEPT = 0x33061849
    BULKPREPARE2 = 0x27feb6ca
    SYNCLOG = 0x6a3fa4b6
    SYNCCOMMIT = 0x12ecba4a
    SYNCNOOPS = 0x6355f3ff
    BULKDECIDE = 0x502582b2

    __input_type_info__ = {
        'Forward': ['MarshallDeputy','uint64_t'],
        'Prepare': ['uint64_t','ballot_t'],
        'Accept': ['uint64_t','uint64_t','ballot_t','MarshallDeputy'],
        'Decide': ['uint64_t','ballot_t','MarshallDeputy'],
        'Heartbeat': ['MarshallDeputy'],
        'ForwardToLearnerServer': ['rrr::i32','uint64_t','ballot_t','MarshallDeputy'],
        'BulkPrepare': ['MarshallDeputy'],
        'BulkAccept': ['MarshallDeputy'],
        'BulkPrepare2': ['MarshallDeputy'],
        'SyncLog': ['MarshallDeputy'],
        'SyncCommit': ['MarshallDeputy'],
        'SyncNoOps': ['MarshallDeputy'],
        'BulkDecide': ['MarshallDeputy'],
    }

    __output_type_info__ = {
        'Forward': ['uint64_t'],
        'Prepare': ['ballot_t','uint64_t'],
        'Accept': ['ballot_t','uint64_t'],
        'Decide': [],
        'Heartbeat': ['rrr::i32','rrr::i32'],
        'ForwardToLearnerServer': ['uint64_t','ballot_t'],
        'BulkPrepare': ['rrr::i32','rrr::i32'],
        'BulkAccept': ['rrr::i32','rrr::i32'],
        'BulkPrepare2': ['rrr::i32','rrr::i32','MarshallDeputy'],
        'SyncLog': ['rrr::i32','rrr::i32','MarshallDeputy'],
        'SyncCommit': ['rrr::i32','rrr::i32'],
        'SyncNoOps': ['rrr::i32','rrr::i32'],
        'BulkDecide': ['rrr::i32','rrr::i32'],
    }

    def __bind_helper__(self, func):
        def f(*args):
            return getattr(self, func.__name__)(*args)
        return f

    def __reg_to__(self, server):
        server.__reg_func__(MultiPaxosService.FORWARD, self.__bind_helper__(self.Forward), ['MarshallDeputy','uint64_t'], ['uint64_t'])
        server.__reg_func__(MultiPaxosService.PREPARE, self.__bind_helper__(self.Prepare), ['uint64_t','ballot_t'], ['ballot_t','uint64_t'])
        server.__reg_func__(MultiPaxosService.ACCEPT, self.__bind_helper__(self.Accept), ['uint64_t','uint64_t','ballot_t','MarshallDeputy'], ['ballot_t','uint64_t'])
        server.__reg_func__(MultiPaxosService.DECIDE, self.__bind_helper__(self.Decide), ['uint64_t','ballot_t','MarshallDeputy'], [])
        server.__reg_func__(MultiPaxosService.HEARTBEAT, self.__bind_helper__(self.Heartbeat), ['MarshallDeputy'], ['rrr::i32','rrr::i32'])
        server.__reg_func__(MultiPaxosService.FORWARDTOLEARNERSERVER, self.__bind_helper__(self.ForwardToLearnerServer), ['rrr::i32','uint64_t','ballot_t','MarshallDeputy'], ['uint64_t','ballot_t'])
        server.__reg_func__(MultiPaxosService.BULKPREPARE, self.__bind_helper__(self.BulkPrepare), ['MarshallDeputy'], ['rrr::i32','rrr::i32'])
        server.__reg_func__(MultiPaxosService.BULKACCEPT, self.__bind_helper__(self.BulkAccept), ['MarshallDeputy'], ['rrr::i32','rrr::i32'])
        server.__reg_func__(MultiPaxosService.BULKPREPARE2, self.__bind_helper__(self.BulkPrepare2), ['MarshallDeputy'], ['rrr::i32','rrr::i32','MarshallDeputy'])
        server.__reg_func__(MultiPaxosService.SYNCLOG, self.__bind_helper__(self.SyncLog), ['MarshallDeputy'], ['rrr::i32','rrr::i32','MarshallDeputy'])
        server.__reg_func__(MultiPaxosService.SYNCCOMMIT, self.__bind_helper__(self.SyncCommit), ['MarshallDeputy'], ['rrr::i32','rrr::i32'])
        server.__reg_func__(MultiPaxosService.SYNCNOOPS, self.__bind_helper__(self.SyncNoOps), ['MarshallDeputy'], ['rrr::i32','rrr::i32'])
        server.__reg_func__(MultiPaxosService.BULKDECIDE, self.__bind_helper__(self.BulkDecide), ['MarshallDeputy'], ['rrr::i32','rrr::i32'])

    def Forward(__self__, cmd, dep_id):
        raise NotImplementedError('subclass MultiPaxosService and implement your own Forward function')

    def Prepare(__self__, slot, ballot):
        raise NotImplementedError('subclass MultiPaxosService and implement your own Prepare function')

    def Accept(__self__, slot, time, ballot, cmd):
        raise NotImplementedError('subclass MultiPaxosService and implement your own Accept function')

    def Decide(__self__, slot, ballot, cmd):
        raise NotImplementedError('subclass MultiPaxosService and implement your own Decide function')

    def Heartbeat(__self__, cmd):
        raise NotImplementedError('subclass MultiPaxosService and implement your own Heartbeat function')

    def ForwardToLearnerServer(__self__, par_id, slot, ballot, cmd):
        raise NotImplementedError('subclass MultiPaxosService and implement your own ForwardToLearnerServer function')

    def BulkPrepare(__self__, cmd):
        raise NotImplementedError('subclass MultiPaxosService and implement your own BulkPrepare function')

    def BulkAccept(__self__, cmd):
        raise NotImplementedError('subclass MultiPaxosService and implement your own BulkAccept function')

    def BulkPrepare2(__self__, cmd):
        raise NotImplementedError('subclass MultiPaxosService and implement your own BulkPrepare2 function')

    def SyncLog(__self__, cmd):
        raise NotImplementedError('subclass MultiPaxosService and implement your own SyncLog function')

    def SyncCommit(__self__, cmd):
        raise NotImplementedError('subclass MultiPaxosService and implement your own SyncCommit function')

    def SyncNoOps(__self__, cmd):
        raise NotImplementedError('subclass MultiPaxosService and implement your own SyncNoOps function')

    def BulkDecide(__self__, cmd):
        raise NotImplementedError('subclass MultiPaxosService and implement your own BulkDecide function')

class MultiPaxosProxy(object):
    def __init__(self, clnt):
        self.__clnt__ = clnt

    def async_Forward(__self__, cmd, dep_id):
        return __self__.__clnt__.async_call(MultiPaxosService.FORWARD, [cmd, dep_id], MultiPaxosService.__input_type_info__['Forward'], MultiPaxosService.__output_type_info__['Forward'])

    def async_Prepare(__self__, slot, ballot):
        return __self__.__clnt__.async_call(MultiPaxosService.PREPARE, [slot, ballot], MultiPaxosService.__input_type_info__['Prepare'], MultiPaxosService.__output_type_info__['Prepare'])

    def async_Accept(__self__, slot, time, ballot, cmd):
        return __self__.__clnt__.async_call(MultiPaxosService.ACCEPT, [slot, time, ballot, cmd], MultiPaxosService.__input_type_info__['Accept'], MultiPaxosService.__output_type_info__['Accept'])

    def async_Decide(__self__, slot, ballot, cmd):
        return __self__.__clnt__.async_call(MultiPaxosService.DECIDE, [slot, ballot, cmd], MultiPaxosService.__input_type_info__['Decide'], MultiPaxosService.__output_type_info__['Decide'])

    def async_Heartbeat(__self__, cmd):
        return __self__.__clnt__.async_call(MultiPaxosService.HEARTBEAT, [cmd], MultiPaxosService.__input_type_info__['Heartbeat'], MultiPaxosService.__output_type_info__['Heartbeat'])

    def async_ForwardToLearnerServer(__self__, par_id, slot, ballot, cmd):
        return __self__.__clnt__.async_call(MultiPaxosService.FORWARDTOLEARNERSERVER, [par_id, slot, ballot, cmd], MultiPaxosService.__input_type_info__['ForwardToLearnerServer'], MultiPaxosService.__output_type_info__['ForwardToLearnerServer'])

    def async_BulkPrepare(__self__, cmd):
        return __self__.__clnt__.async_call(MultiPaxosService.BULKPREPARE, [cmd], MultiPaxosService.__input_type_info__['BulkPrepare'], MultiPaxosService.__output_type_info__['BulkPrepare'])

    def async_BulkAccept(__self__, cmd):
        return __self__.__clnt__.async_call(MultiPaxosService.BULKACCEPT, [cmd], MultiPaxosService.__input_type_info__['BulkAccept'], MultiPaxosService.__output_type_info__['BulkAccept'])

    def async_BulkPrepare2(__self__, cmd):
        return __self__.__clnt__.async_call(MultiPaxosService.BULKPREPARE2, [cmd], MultiPaxosService.__input_type_info__['BulkPrepare2'], MultiPaxosService.__output_type_info__['BulkPrepare2'])

    def async_SyncLog(__self__, cmd):
        return __self__.__clnt__.async_call(MultiPaxosService.SYNCLOG, [cmd], MultiPaxosService.__input_type_info__['SyncLog'], MultiPaxosService.__output_type_info__['SyncLog'])

    def async_SyncCommit(__self__, cmd):
        return __self__.__clnt__.async_call(MultiPaxosService.SYNCCOMMIT, [cmd], MultiPaxosService.__input_type_info__['SyncCommit'], MultiPaxosService.__output_type_info__['SyncCommit'])

    def async_SyncNoOps(__self__, cmd):
        return __self__.__clnt__.async_call(MultiPaxosService.SYNCNOOPS, [cmd], MultiPaxosService.__input_type_info__['SyncNoOps'], MultiPaxosService.__output_type_info__['SyncNoOps'])

    def async_BulkDecide(__self__, cmd):
        return __self__.__clnt__.async_call(MultiPaxosService.BULKDECIDE, [cmd], MultiPaxosService.__input_type_info__['BulkDecide'], MultiPaxosService.__output_type_info__['BulkDecide'])

    def sync_Forward(__self__, cmd, dep_id):
        __result__ = __self__.__clnt__.sync_call(MultiPaxosService.FORWARD, [cmd, dep_id], MultiPaxosService.__input_type_info__['Forward'], MultiPaxosService.__output_type_info__['Forward'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_Prepare(__self__, slot, ballot):
        __result__ = __self__.__clnt__.sync_call(MultiPaxosService.PREPARE, [slot, ballot], MultiPaxosService.__input_type_info__['Prepare'], MultiPaxosService.__output_type_info__['Prepare'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_Accept(__self__, slot, time, ballot, cmd):
        __result__ = __self__.__clnt__.sync_call(MultiPaxosService.ACCEPT, [slot, time, ballot, cmd], MultiPaxosService.__input_type_info__['Accept'], MultiPaxosService.__output_type_info__['Accept'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_Decide(__self__, slot, ballot, cmd):
        __result__ = __self__.__clnt__.sync_call(MultiPaxosService.DECIDE, [slot, ballot, cmd], MultiPaxosService.__input_type_info__['Decide'], MultiPaxosService.__output_type_info__['Decide'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_Heartbeat(__self__, cmd):
        __result__ = __self__.__clnt__.sync_call(MultiPaxosService.HEARTBEAT, [cmd], MultiPaxosService.__input_type_info__['Heartbeat'], MultiPaxosService.__output_type_info__['Heartbeat'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_ForwardToLearnerServer(__self__, par_id, slot, ballot, cmd):
        __result__ = __self__.__clnt__.sync_call(MultiPaxosService.FORWARDTOLEARNERSERVER, [par_id, slot, ballot, cmd], MultiPaxosService.__input_type_info__['ForwardToLearnerServer'], MultiPaxosService.__output_type_info__['ForwardToLearnerServer'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_BulkPrepare(__self__, cmd):
        __result__ = __self__.__clnt__.sync_call(MultiPaxosService.BULKPREPARE, [cmd], MultiPaxosService.__input_type_info__['BulkPrepare'], MultiPaxosService.__output_type_info__['BulkPrepare'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_BulkAccept(__self__, cmd):
        __result__ = __self__.__clnt__.sync_call(MultiPaxosService.BULKACCEPT, [cmd], MultiPaxosService.__input_type_info__['BulkAccept'], MultiPaxosService.__output_type_info__['BulkAccept'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_BulkPrepare2(__self__, cmd):
        __result__ = __self__.__clnt__.sync_call(MultiPaxosService.BULKPREPARE2, [cmd], MultiPaxosService.__input_type_info__['BulkPrepare2'], MultiPaxosService.__output_type_info__['BulkPrepare2'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_SyncLog(__self__, cmd):
        __result__ = __self__.__clnt__.sync_call(MultiPaxosService.SYNCLOG, [cmd], MultiPaxosService.__input_type_info__['SyncLog'], MultiPaxosService.__output_type_info__['SyncLog'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_SyncCommit(__self__, cmd):
        __result__ = __self__.__clnt__.sync_call(MultiPaxosService.SYNCCOMMIT, [cmd], MultiPaxosService.__input_type_info__['SyncCommit'], MultiPaxosService.__output_type_info__['SyncCommit'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_SyncNoOps(__self__, cmd):
        __result__ = __self__.__clnt__.sync_call(MultiPaxosService.SYNCNOOPS, [cmd], MultiPaxosService.__input_type_info__['SyncNoOps'], MultiPaxosService.__output_type_info__['SyncNoOps'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_BulkDecide(__self__, cmd):
        __result__ = __self__.__clnt__.sync_call(MultiPaxosService.BULKDECIDE, [cmd], MultiPaxosService.__input_type_info__['BulkDecide'], MultiPaxosService.__output_type_info__['BulkDecide'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

class MongodbService(object):
    COMMIT = 0x55a97b80

    __input_type_info__ = {
        'Commit': ['MarshallDeputy'],
    }

    __output_type_info__ = {
        'Commit': [],
    }

    def __bind_helper__(self, func):
        def f(*args):
            return getattr(self, func.__name__)(*args)
        return f

    def __reg_to__(self, server):
        server.__reg_func__(MongodbService.COMMIT, self.__bind_helper__(self.Commit), ['MarshallDeputy'], [])

    def Commit(__self__, cmd):
        raise NotImplementedError('subclass MongodbService and implement your own Commit function')

class MongodbProxy(object):
    def __init__(self, clnt):
        self.__clnt__ = clnt

    def async_Commit(__self__, cmd):
        return __self__.__clnt__.async_call(MongodbService.COMMIT, [cmd], MongodbService.__input_type_info__['Commit'], MongodbService.__output_type_info__['Commit'])

    def sync_Commit(__self__, cmd):
        __result__ = __self__.__clnt__.sync_call(MongodbService.COMMIT, [cmd], MongodbService.__input_type_info__['Commit'], MongodbService.__output_type_info__['Commit'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

class MenciusService(object):
    PREPARE = 0x49fb16e1
    SUGGEST = 0x1115ddd5
    DECIDE = 0x6a2766df

    __input_type_info__ = {
        'Prepare': ['uint64_t','ballot_t'],
        'Suggest': ['uint64_t','uint64_t','ballot_t','uint64_t','std::vector<uint64_t>','std::vector<uint64_t>','MarshallDeputy'],
        'Decide': ['uint64_t','ballot_t','MarshallDeputy'],
    }

    __output_type_info__ = {
        'Prepare': ['ballot_t','uint64_t'],
        'Suggest': ['ballot_t','uint64_t'],
        'Decide': [],
    }

    def __bind_helper__(self, func):
        def f(*args):
            return getattr(self, func.__name__)(*args)
        return f

    def __reg_to__(self, server):
        server.__reg_func__(MenciusService.PREPARE, self.__bind_helper__(self.Prepare), ['uint64_t','ballot_t'], ['ballot_t','uint64_t'])
        server.__reg_func__(MenciusService.SUGGEST, self.__bind_helper__(self.Suggest), ['uint64_t','uint64_t','ballot_t','uint64_t','std::vector<uint64_t>','std::vector<uint64_t>','MarshallDeputy'], ['ballot_t','uint64_t'])
        server.__reg_func__(MenciusService.DECIDE, self.__bind_helper__(self.Decide), ['uint64_t','ballot_t','MarshallDeputy'], [])

    def Prepare(__self__, slot, ballot):
        raise NotImplementedError('subclass MenciusService and implement your own Prepare function')

    def Suggest(__self__, slot, time, ballot, sender, skip_commits, skip_potentials, cmd):
        raise NotImplementedError('subclass MenciusService and implement your own Suggest function')

    def Decide(__self__, slot, ballot, cmd):
        raise NotImplementedError('subclass MenciusService and implement your own Decide function')

class MenciusProxy(object):
    def __init__(self, clnt):
        self.__clnt__ = clnt

    def async_Prepare(__self__, slot, ballot):
        return __self__.__clnt__.async_call(MenciusService.PREPARE, [slot, ballot], MenciusService.__input_type_info__['Prepare'], MenciusService.__output_type_info__['Prepare'])

    def async_Suggest(__self__, slot, time, ballot, sender, skip_commits, skip_potentials, cmd):
        return __self__.__clnt__.async_call(MenciusService.SUGGEST, [slot, time, ballot, sender, skip_commits, skip_potentials, cmd], MenciusService.__input_type_info__['Suggest'], MenciusService.__output_type_info__['Suggest'])

    def async_Decide(__self__, slot, ballot, cmd):
        return __self__.__clnt__.async_call(MenciusService.DECIDE, [slot, ballot, cmd], MenciusService.__input_type_info__['Decide'], MenciusService.__output_type_info__['Decide'])

    def sync_Prepare(__self__, slot, ballot):
        __result__ = __self__.__clnt__.sync_call(MenciusService.PREPARE, [slot, ballot], MenciusService.__input_type_info__['Prepare'], MenciusService.__output_type_info__['Prepare'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_Suggest(__self__, slot, time, ballot, sender, skip_commits, skip_potentials, cmd):
        __result__ = __self__.__clnt__.sync_call(MenciusService.SUGGEST, [slot, time, ballot, sender, skip_commits, skip_potentials, cmd], MenciusService.__input_type_info__['Suggest'], MenciusService.__output_type_info__['Suggest'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_Decide(__self__, slot, ballot, cmd):
        __result__ = __self__.__clnt__.sync_call(MenciusService.DECIDE, [slot, ballot, cmd], MenciusService.__input_type_info__['Decide'], MenciusService.__output_type_info__['Decide'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

class FpgaRaftService(object):
    HEARTBEAT = 0x6ba1c6e8
    FORWARD = 0x1ff9d492
    VOTE = 0x34f566a7
    VOTE2FPGA = 0x1044e8ba
    APPENDENTRIES = 0x6ef69347
    APPENDENTRIES2 = 0x59858654
    DECIDE = 0x5b295809

    __input_type_info__ = {
        'Heartbeat': ['uint64_t','DepId'],
        'Forward': ['MarshallDeputy'],
        'Vote': ['uint64_t','ballot_t','parid_t','ballot_t'],
        'Vote2FPGA': ['uint64_t','ballot_t','parid_t','ballot_t'],
        'AppendEntries': ['uint64_t','ballot_t','uint64_t','uint64_t','uint64_t','uint64_t','DepId','MarshallDeputy'],
        'AppendEntries2': ['uint64_t','ballot_t','uint64_t','uint64_t','uint64_t','uint64_t','DepId','MarshallDeputy'],
        'Decide': ['uint64_t','ballot_t','DepId','MarshallDeputy'],
    }

    __output_type_info__ = {
        'Heartbeat': ['uint64_t'],
        'Forward': ['uint64_t'],
        'Vote': ['ballot_t','bool_t'],
        'Vote2FPGA': ['ballot_t','bool_t'],
        'AppendEntries': ['uint64_t','uint64_t','uint64_t'],
        'AppendEntries2': ['uint64_t','uint64_t','uint64_t'],
        'Decide': [],
    }

    def __bind_helper__(self, func):
        def f(*args):
            return getattr(self, func.__name__)(*args)
        return f

    def __reg_to__(self, server):
        server.__reg_func__(FpgaRaftService.HEARTBEAT, self.__bind_helper__(self.Heartbeat), ['uint64_t','DepId'], ['uint64_t'])
        server.__reg_func__(FpgaRaftService.FORWARD, self.__bind_helper__(self.Forward), ['MarshallDeputy'], ['uint64_t'])
        server.__reg_func__(FpgaRaftService.VOTE, self.__bind_helper__(self.Vote), ['uint64_t','ballot_t','parid_t','ballot_t'], ['ballot_t','bool_t'])
        server.__reg_func__(FpgaRaftService.VOTE2FPGA, self.__bind_helper__(self.Vote2FPGA), ['uint64_t','ballot_t','parid_t','ballot_t'], ['ballot_t','bool_t'])
        server.__reg_func__(FpgaRaftService.APPENDENTRIES, self.__bind_helper__(self.AppendEntries), ['uint64_t','ballot_t','uint64_t','uint64_t','uint64_t','uint64_t','DepId','MarshallDeputy'], ['uint64_t','uint64_t','uint64_t'])
        server.__reg_func__(FpgaRaftService.APPENDENTRIES2, self.__bind_helper__(self.AppendEntries2), ['uint64_t','ballot_t','uint64_t','uint64_t','uint64_t','uint64_t','DepId','MarshallDeputy'], ['uint64_t','uint64_t','uint64_t'])
        server.__reg_func__(FpgaRaftService.DECIDE, self.__bind_helper__(self.Decide), ['uint64_t','ballot_t','DepId','MarshallDeputy'], [])

    def Heartbeat(__self__, leaderPrevLogIndex, dep_id):
        raise NotImplementedError('subclass FpgaRaftService and implement your own Heartbeat function')

    def Forward(__self__, cmd):
        raise NotImplementedError('subclass FpgaRaftService and implement your own Forward function')

    def Vote(__self__, lst_log_idx, lst_log_term, par_id, cur_term):
        raise NotImplementedError('subclass FpgaRaftService and implement your own Vote function')

    def Vote2FPGA(__self__, lst_log_idx, lst_log_term, par_id, cur_term):
        raise NotImplementedError('subclass FpgaRaftService and implement your own Vote2FPGA function')

    def AppendEntries(__self__, slot, ballot, leaderCurrentTerm, leaderPrevLogIndex, leaderPrevLogTerm, leaderCommitIndex, dep_id, cmd):
        raise NotImplementedError('subclass FpgaRaftService and implement your own AppendEntries function')

    def AppendEntries2(__self__, slot, ballot, leaderCurrentTerm, leaderPrevLogIndex, leaderPrevLogTerm, leaderCommitIndex, dep_id, cmd):
        raise NotImplementedError('subclass FpgaRaftService and implement your own AppendEntries2 function')

    def Decide(__self__, slot, ballot, dep_id, cmd):
        raise NotImplementedError('subclass FpgaRaftService and implement your own Decide function')

class FpgaRaftProxy(object):
    def __init__(self, clnt):
        self.__clnt__ = clnt

    def async_Heartbeat(__self__, leaderPrevLogIndex, dep_id):
        return __self__.__clnt__.async_call(FpgaRaftService.HEARTBEAT, [leaderPrevLogIndex, dep_id], FpgaRaftService.__input_type_info__['Heartbeat'], FpgaRaftService.__output_type_info__['Heartbeat'])

    def async_Forward(__self__, cmd):
        return __self__.__clnt__.async_call(FpgaRaftService.FORWARD, [cmd], FpgaRaftService.__input_type_info__['Forward'], FpgaRaftService.__output_type_info__['Forward'])

    def async_Vote(__self__, lst_log_idx, lst_log_term, par_id, cur_term):
        return __self__.__clnt__.async_call(FpgaRaftService.VOTE, [lst_log_idx, lst_log_term, par_id, cur_term], FpgaRaftService.__input_type_info__['Vote'], FpgaRaftService.__output_type_info__['Vote'])

    def async_Vote2FPGA(__self__, lst_log_idx, lst_log_term, par_id, cur_term):
        return __self__.__clnt__.async_call(FpgaRaftService.VOTE2FPGA, [lst_log_idx, lst_log_term, par_id, cur_term], FpgaRaftService.__input_type_info__['Vote2FPGA'], FpgaRaftService.__output_type_info__['Vote2FPGA'])

    def async_AppendEntries(__self__, slot, ballot, leaderCurrentTerm, leaderPrevLogIndex, leaderPrevLogTerm, leaderCommitIndex, dep_id, cmd):
        return __self__.__clnt__.async_call(FpgaRaftService.APPENDENTRIES, [slot, ballot, leaderCurrentTerm, leaderPrevLogIndex, leaderPrevLogTerm, leaderCommitIndex, dep_id, cmd], FpgaRaftService.__input_type_info__['AppendEntries'], FpgaRaftService.__output_type_info__['AppendEntries'])

    def async_AppendEntries2(__self__, slot, ballot, leaderCurrentTerm, leaderPrevLogIndex, leaderPrevLogTerm, leaderCommitIndex, dep_id, cmd):
        return __self__.__clnt__.async_call(FpgaRaftService.APPENDENTRIES2, [slot, ballot, leaderCurrentTerm, leaderPrevLogIndex, leaderPrevLogTerm, leaderCommitIndex, dep_id, cmd], FpgaRaftService.__input_type_info__['AppendEntries2'], FpgaRaftService.__output_type_info__['AppendEntries2'])

    def async_Decide(__self__, slot, ballot, dep_id, cmd):
        return __self__.__clnt__.async_call(FpgaRaftService.DECIDE, [slot, ballot, dep_id, cmd], FpgaRaftService.__input_type_info__['Decide'], FpgaRaftService.__output_type_info__['Decide'])

    def sync_Heartbeat(__self__, leaderPrevLogIndex, dep_id):
        __result__ = __self__.__clnt__.sync_call(FpgaRaftService.HEARTBEAT, [leaderPrevLogIndex, dep_id], FpgaRaftService.__input_type_info__['Heartbeat'], FpgaRaftService.__output_type_info__['Heartbeat'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_Forward(__self__, cmd):
        __result__ = __self__.__clnt__.sync_call(FpgaRaftService.FORWARD, [cmd], FpgaRaftService.__input_type_info__['Forward'], FpgaRaftService.__output_type_info__['Forward'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_Vote(__self__, lst_log_idx, lst_log_term, par_id, cur_term):
        __result__ = __self__.__clnt__.sync_call(FpgaRaftService.VOTE, [lst_log_idx, lst_log_term, par_id, cur_term], FpgaRaftService.__input_type_info__['Vote'], FpgaRaftService.__output_type_info__['Vote'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_Vote2FPGA(__self__, lst_log_idx, lst_log_term, par_id, cur_term):
        __result__ = __self__.__clnt__.sync_call(FpgaRaftService.VOTE2FPGA, [lst_log_idx, lst_log_term, par_id, cur_term], FpgaRaftService.__input_type_info__['Vote2FPGA'], FpgaRaftService.__output_type_info__['Vote2FPGA'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_AppendEntries(__self__, slot, ballot, leaderCurrentTerm, leaderPrevLogIndex, leaderPrevLogTerm, leaderCommitIndex, dep_id, cmd):
        __result__ = __self__.__clnt__.sync_call(FpgaRaftService.APPENDENTRIES, [slot, ballot, leaderCurrentTerm, leaderPrevLogIndex, leaderPrevLogTerm, leaderCommitIndex, dep_id, cmd], FpgaRaftService.__input_type_info__['AppendEntries'], FpgaRaftService.__output_type_info__['AppendEntries'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_AppendEntries2(__self__, slot, ballot, leaderCurrentTerm, leaderPrevLogIndex, leaderPrevLogTerm, leaderCommitIndex, dep_id, cmd):
        __result__ = __self__.__clnt__.sync_call(FpgaRaftService.APPENDENTRIES2, [slot, ballot, leaderCurrentTerm, leaderPrevLogIndex, leaderPrevLogTerm, leaderCommitIndex, dep_id, cmd], FpgaRaftService.__input_type_info__['AppendEntries2'], FpgaRaftService.__output_type_info__['AppendEntries2'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_Decide(__self__, slot, ballot, dep_id, cmd):
        __result__ = __self__.__clnt__.sync_call(FpgaRaftService.DECIDE, [slot, ballot, dep_id, cmd], FpgaRaftService.__input_type_info__['Decide'], FpgaRaftService.__output_type_info__['Decide'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

class RaftService(object):
    VOTE = 0x4003b73d
    VOTEDURABLE = 0x10c80916
    APPENDENTRIES = 0x110a36e9
    EMPTYAPPENDENTRIES = 0x1f4ee6a6
    APPENDENTRIESDURABLE = 0x250acaf9
    TIMEOUTNOW = 0x27a16a23
    NOTIFYRESTART = 0x532f8698

    __input_type_info__ = {
        'Vote': ['uint64_t','ballot_t','siteid_t','ballot_t'],
        'VoteDurable': ['ballot_t','siteid_t'],
        'AppendEntries': ['uint64_t','ballot_t','uint64_t','siteid_t','uint64_t','uint64_t','uint64_t','MarshallDeputy','uint64_t'],
        'EmptyAppendEntries': ['uint64_t','ballot_t','uint64_t','siteid_t','uint64_t','uint64_t','uint64_t','bool_t'],
        'AppendEntriesDurable': ['ballot_t','siteid_t','uint64_t'],
        'TimeoutNow': ['uint64_t','siteid_t'],
        'NotifyRestart': ['siteid_t'],
    }

    __output_type_info__ = {
        'Vote': ['ballot_t','bool_t'],
        'VoteDurable': ['bool_t'],
        'AppendEntries': ['uint64_t','uint64_t','uint64_t','uint64_t'],
        'EmptyAppendEntries': ['uint64_t','uint64_t','uint64_t','uint64_t'],
        'AppendEntriesDurable': ['bool_t'],
        'TimeoutNow': ['uint64_t','bool_t'],
        'NotifyRestart': ['bool_t'],
    }

    def __bind_helper__(self, func):
        def f(*args):
            return getattr(self, func.__name__)(*args)
        return f

    def __reg_to__(self, server):
        server.__reg_func__(RaftService.VOTE, self.__bind_helper__(self.Vote), ['uint64_t','ballot_t','siteid_t','ballot_t'], ['ballot_t','bool_t'])
        server.__reg_func__(RaftService.VOTEDURABLE, self.__bind_helper__(self.VoteDurable), ['ballot_t','siteid_t'], ['bool_t'])
        server.__reg_func__(RaftService.APPENDENTRIES, self.__bind_helper__(self.AppendEntries), ['uint64_t','ballot_t','uint64_t','siteid_t','uint64_t','uint64_t','uint64_t','MarshallDeputy','uint64_t'], ['uint64_t','uint64_t','uint64_t','uint64_t'])
        server.__reg_func__(RaftService.EMPTYAPPENDENTRIES, self.__bind_helper__(self.EmptyAppendEntries), ['uint64_t','ballot_t','uint64_t','siteid_t','uint64_t','uint64_t','uint64_t','bool_t'], ['uint64_t','uint64_t','uint64_t','uint64_t'])
        server.__reg_func__(RaftService.APPENDENTRIESDURABLE, self.__bind_helper__(self.AppendEntriesDurable), ['ballot_t','siteid_t','uint64_t'], ['bool_t'])
        server.__reg_func__(RaftService.TIMEOUTNOW, self.__bind_helper__(self.TimeoutNow), ['uint64_t','siteid_t'], ['uint64_t','bool_t'])
        server.__reg_func__(RaftService.NOTIFYRESTART, self.__bind_helper__(self.NotifyRestart), ['siteid_t'], ['bool_t'])

    def Vote(__self__, lst_log_idx, lst_log_term, site_id, cur_term):
        raise NotImplementedError('subclass RaftService and implement your own Vote function')

    def VoteDurable(__self__, term, voter_id):
        raise NotImplementedError('subclass RaftService and implement your own VoteDurable function')

    def AppendEntries(__self__, slot, ballot, leaderCurrentTerm, leaderSiteId, leaderPrevLogIndex, leaderPrevLogTerm, leaderCommitIndex, cmd, leaderNextLogTerm):
        raise NotImplementedError('subclass RaftService and implement your own AppendEntries function')

    def EmptyAppendEntries(__self__, slot, ballot, leaderCurrentTerm, leaderSiteId, leaderPrevLogIndex, leaderPrevLogTerm, leaderCommitIndex, trigger_election_now):
        raise NotImplementedError('subclass RaftService and implement your own EmptyAppendEntries function')

    def AppendEntriesDurable(__self__, term, follower_id, lastLogIndex):
        raise NotImplementedError('subclass RaftService and implement your own AppendEntriesDurable function')

    def TimeoutNow(__self__, leaderTerm, leaderSiteId):
        raise NotImplementedError('subclass RaftService and implement your own TimeoutNow function')

    def NotifyRestart(__self__, restartedSiteId):
        raise NotImplementedError('subclass RaftService and implement your own NotifyRestart function')

class RaftProxy(object):
    def __init__(self, clnt):
        self.__clnt__ = clnt

    def async_Vote(__self__, lst_log_idx, lst_log_term, site_id, cur_term):
        return __self__.__clnt__.async_call(RaftService.VOTE, [lst_log_idx, lst_log_term, site_id, cur_term], RaftService.__input_type_info__['Vote'], RaftService.__output_type_info__['Vote'])

    def async_VoteDurable(__self__, term, voter_id):
        return __self__.__clnt__.async_call(RaftService.VOTEDURABLE, [term, voter_id], RaftService.__input_type_info__['VoteDurable'], RaftService.__output_type_info__['VoteDurable'])

    def async_AppendEntries(__self__, slot, ballot, leaderCurrentTerm, leaderSiteId, leaderPrevLogIndex, leaderPrevLogTerm, leaderCommitIndex, cmd, leaderNextLogTerm):
        return __self__.__clnt__.async_call(RaftService.APPENDENTRIES, [slot, ballot, leaderCurrentTerm, leaderSiteId, leaderPrevLogIndex, leaderPrevLogTerm, leaderCommitIndex, cmd, leaderNextLogTerm], RaftService.__input_type_info__['AppendEntries'], RaftService.__output_type_info__['AppendEntries'])

    def async_EmptyAppendEntries(__self__, slot, ballot, leaderCurrentTerm, leaderSiteId, leaderPrevLogIndex, leaderPrevLogTerm, leaderCommitIndex, trigger_election_now):
        return __self__.__clnt__.async_call(RaftService.EMPTYAPPENDENTRIES, [slot, ballot, leaderCurrentTerm, leaderSiteId, leaderPrevLogIndex, leaderPrevLogTerm, leaderCommitIndex, trigger_election_now], RaftService.__input_type_info__['EmptyAppendEntries'], RaftService.__output_type_info__['EmptyAppendEntries'])

    def async_AppendEntriesDurable(__self__, term, follower_id, lastLogIndex):
        return __self__.__clnt__.async_call(RaftService.APPENDENTRIESDURABLE, [term, follower_id, lastLogIndex], RaftService.__input_type_info__['AppendEntriesDurable'], RaftService.__output_type_info__['AppendEntriesDurable'])

    def async_TimeoutNow(__self__, leaderTerm, leaderSiteId):
        return __self__.__clnt__.async_call(RaftService.TIMEOUTNOW, [leaderTerm, leaderSiteId], RaftService.__input_type_info__['TimeoutNow'], RaftService.__output_type_info__['TimeoutNow'])

    def async_NotifyRestart(__self__, restartedSiteId):
        return __self__.__clnt__.async_call(RaftService.NOTIFYRESTART, [restartedSiteId], RaftService.__input_type_info__['NotifyRestart'], RaftService.__output_type_info__['NotifyRestart'])

    def sync_Vote(__self__, lst_log_idx, lst_log_term, site_id, cur_term):
        __result__ = __self__.__clnt__.sync_call(RaftService.VOTE, [lst_log_idx, lst_log_term, site_id, cur_term], RaftService.__input_type_info__['Vote'], RaftService.__output_type_info__['Vote'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_VoteDurable(__self__, term, voter_id):
        __result__ = __self__.__clnt__.sync_call(RaftService.VOTEDURABLE, [term, voter_id], RaftService.__input_type_info__['VoteDurable'], RaftService.__output_type_info__['VoteDurable'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_AppendEntries(__self__, slot, ballot, leaderCurrentTerm, leaderSiteId, leaderPrevLogIndex, leaderPrevLogTerm, leaderCommitIndex, cmd, leaderNextLogTerm):
        __result__ = __self__.__clnt__.sync_call(RaftService.APPENDENTRIES, [slot, ballot, leaderCurrentTerm, leaderSiteId, leaderPrevLogIndex, leaderPrevLogTerm, leaderCommitIndex, cmd, leaderNextLogTerm], RaftService.__input_type_info__['AppendEntries'], RaftService.__output_type_info__['AppendEntries'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_EmptyAppendEntries(__self__, slot, ballot, leaderCurrentTerm, leaderSiteId, leaderPrevLogIndex, leaderPrevLogTerm, leaderCommitIndex, trigger_election_now):
        __result__ = __self__.__clnt__.sync_call(RaftService.EMPTYAPPENDENTRIES, [slot, ballot, leaderCurrentTerm, leaderSiteId, leaderPrevLogIndex, leaderPrevLogTerm, leaderCommitIndex, trigger_election_now], RaftService.__input_type_info__['EmptyAppendEntries'], RaftService.__output_type_info__['EmptyAppendEntries'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_AppendEntriesDurable(__self__, term, follower_id, lastLogIndex):
        __result__ = __self__.__clnt__.sync_call(RaftService.APPENDENTRIESDURABLE, [term, follower_id, lastLogIndex], RaftService.__input_type_info__['AppendEntriesDurable'], RaftService.__output_type_info__['AppendEntriesDurable'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_TimeoutNow(__self__, leaderTerm, leaderSiteId):
        __result__ = __self__.__clnt__.sync_call(RaftService.TIMEOUTNOW, [leaderTerm, leaderSiteId], RaftService.__input_type_info__['TimeoutNow'], RaftService.__output_type_info__['TimeoutNow'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_NotifyRestart(__self__, restartedSiteId):
        __result__ = __self__.__clnt__.sync_call(RaftService.NOTIFYRESTART, [restartedSiteId], RaftService.__input_type_info__['NotifyRestart'], RaftService.__output_type_info__['NotifyRestart'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

class CopilotService(object):
    FORWARD = 0x16c5d7c0
    PREPARE = 0x411de4a4
    FASTACCEPT = 0x11bfd36b
    ACCEPT = 0x6f5ed10d
    COMMIT = 0x19cc7449

    __input_type_info__ = {
        'Forward': ['MarshallDeputy'],
        'Prepare': ['uint8_t','uint64_t','ballot_t','DepId'],
        'FastAccept': ['uint8_t','uint64_t','ballot_t','uint64_t','MarshallDeputy','DepId'],
        'Accept': ['uint8_t','uint64_t','ballot_t','uint64_t','MarshallDeputy','DepId'],
        'Commit': ['uint8_t','uint64_t','uint64_t','MarshallDeputy'],
    }

    __output_type_info__ = {
        'Forward': [],
        'Prepare': ['MarshallDeputy','ballot_t','uint64_t','status_t'],
        'FastAccept': ['ballot_t','uint64_t'],
        'Accept': ['ballot_t'],
        'Commit': [],
    }

    def __bind_helper__(self, func):
        def f(*args):
            return getattr(self, func.__name__)(*args)
        return f

    def __reg_to__(self, server):
        server.__reg_func__(CopilotService.FORWARD, self.__bind_helper__(self.Forward), ['MarshallDeputy'], [])
        server.__reg_func__(CopilotService.PREPARE, self.__bind_helper__(self.Prepare), ['uint8_t','uint64_t','ballot_t','DepId'], ['MarshallDeputy','ballot_t','uint64_t','status_t'])
        server.__reg_func__(CopilotService.FASTACCEPT, self.__bind_helper__(self.FastAccept), ['uint8_t','uint64_t','ballot_t','uint64_t','MarshallDeputy','DepId'], ['ballot_t','uint64_t'])
        server.__reg_func__(CopilotService.ACCEPT, self.__bind_helper__(self.Accept), ['uint8_t','uint64_t','ballot_t','uint64_t','MarshallDeputy','DepId'], ['ballot_t'])
        server.__reg_func__(CopilotService.COMMIT, self.__bind_helper__(self.Commit), ['uint8_t','uint64_t','uint64_t','MarshallDeputy'], [])

    def Forward(__self__, cmd):
        raise NotImplementedError('subclass CopilotService and implement your own Forward function')

    def Prepare(__self__, is_pilot, slot, ballot, dep_id):
        raise NotImplementedError('subclass CopilotService and implement your own Prepare function')

    def FastAccept(__self__, is_pilot, slot, ballot, dep, cmd, dep_id):
        raise NotImplementedError('subclass CopilotService and implement your own FastAccept function')

    def Accept(__self__, is_pilot, slot, ballot, dep, cmd, dep_id):
        raise NotImplementedError('subclass CopilotService and implement your own Accept function')

    def Commit(__self__, is_pilot, slot, dep, cmd):
        raise NotImplementedError('subclass CopilotService and implement your own Commit function')

class CopilotProxy(object):
    def __init__(self, clnt):
        self.__clnt__ = clnt

    def async_Forward(__self__, cmd):
        return __self__.__clnt__.async_call(CopilotService.FORWARD, [cmd], CopilotService.__input_type_info__['Forward'], CopilotService.__output_type_info__['Forward'])

    def async_Prepare(__self__, is_pilot, slot, ballot, dep_id):
        return __self__.__clnt__.async_call(CopilotService.PREPARE, [is_pilot, slot, ballot, dep_id], CopilotService.__input_type_info__['Prepare'], CopilotService.__output_type_info__['Prepare'])

    def async_FastAccept(__self__, is_pilot, slot, ballot, dep, cmd, dep_id):
        return __self__.__clnt__.async_call(CopilotService.FASTACCEPT, [is_pilot, slot, ballot, dep, cmd, dep_id], CopilotService.__input_type_info__['FastAccept'], CopilotService.__output_type_info__['FastAccept'])

    def async_Accept(__self__, is_pilot, slot, ballot, dep, cmd, dep_id):
        return __self__.__clnt__.async_call(CopilotService.ACCEPT, [is_pilot, slot, ballot, dep, cmd, dep_id], CopilotService.__input_type_info__['Accept'], CopilotService.__output_type_info__['Accept'])

    def async_Commit(__self__, is_pilot, slot, dep, cmd):
        return __self__.__clnt__.async_call(CopilotService.COMMIT, [is_pilot, slot, dep, cmd], CopilotService.__input_type_info__['Commit'], CopilotService.__output_type_info__['Commit'])

    def sync_Forward(__self__, cmd):
        __result__ = __self__.__clnt__.sync_call(CopilotService.FORWARD, [cmd], CopilotService.__input_type_info__['Forward'], CopilotService.__output_type_info__['Forward'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_Prepare(__self__, is_pilot, slot, ballot, dep_id):
        __result__ = __self__.__clnt__.sync_call(CopilotService.PREPARE, [is_pilot, slot, ballot, dep_id], CopilotService.__input_type_info__['Prepare'], CopilotService.__output_type_info__['Prepare'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_FastAccept(__self__, is_pilot, slot, ballot, dep, cmd, dep_id):
        __result__ = __self__.__clnt__.sync_call(CopilotService.FASTACCEPT, [is_pilot, slot, ballot, dep, cmd, dep_id], CopilotService.__input_type_info__['FastAccept'], CopilotService.__output_type_info__['FastAccept'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_Accept(__self__, is_pilot, slot, ballot, dep, cmd, dep_id):
        __result__ = __self__.__clnt__.sync_call(CopilotService.ACCEPT, [is_pilot, slot, ballot, dep, cmd, dep_id], CopilotService.__input_type_info__['Accept'], CopilotService.__output_type_info__['Accept'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_Commit(__self__, is_pilot, slot, dep, cmd):
        __result__ = __self__.__clnt__.sync_call(CopilotService.COMMIT, [is_pilot, slot, dep, cmd], CopilotService.__input_type_info__['Commit'], CopilotService.__output_type_info__['Commit'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

class ClassicService(object):
    MSGSTRING = 0x3c6eeb6d
    MSGMARSHALL = 0x26687927
    REELECT = 0x502410be
    RULESPECULATIVEEXECUTE = 0x30b406fb
    DISPATCH = 0x13049a54
    PREPARE = 0x3a1a20a4
    COMMIT = 0x217dfdf5
    ABORT = 0x4718ac05
    EARLYABORT = 0x4568c33f
    UPGRADEEPOCH = 0x4b17a501
    TRUNCATEEPOCH = 0x55d6fca1
    ISLEADER = 0x66da29b7
    ISFPGALEADER = 0x5a20b693
    SIMPLECMD = 0x5e873c88
    FAILOVERPAUSESOCKETOUT = 0x1819f7ab
    FAILOVERRESUMESOCKETOUT = 0x28663bec
    RPC_NULL = 0x1d5df782
    TAPIRACCEPT = 0x3efa1ec5
    TAPIRFASTACCEPT = 0x55f995c6
    TAPIRDECIDE = 0x5b081ffc
    CAROUSELREADANDPREPARE = 0x4f278ebe
    CAROUSELACCEPT = 0x213fcd9d
    CAROUSELFASTACCEPT = 0x6f9b357e
    CAROUSELDECIDE = 0x4bd3da88
    RCCDISPATCH = 0x6b7978d7
    RCCFINISH = 0x53357087
    RCCINQUIRE = 0x5beaaa97
    RCCDISPATCHRO = 0x1695c037
    RCCINQUIREVALIDATION = 0x3e58233c
    RCCNOTIFYGLOBALVALIDATION = 0x5e2cd919
    JANUSDISPATCH = 0x2de24a44
    RCCCOMMIT = 0x30b11969
    JANUSCOMMIT = 0x458804fe
    JANUSCOMMITWOGRAPH = 0x3180c392
    JANUSINQUIRE = 0x46943b47
    RCCPREACCEPT = 0x376c6809
    JANUSPREACCEPT = 0x230d7fe2
    JANUSPREACCEPTWOGRAPH = 0x51579da7
    RCCACCEPT = 0x4c25e499
    JANUSACCEPT = 0x1abd8b2f
    PREACCEPTFEBRUUS = 0x2a0491a0
    ACCEPTFEBRUUS = 0x60098496
    COMMITFEBRUUS = 0x2128dbce
    JETPACKBEGINRECOVERY = 0x3b3b4651
    JETPACKPULLIDSET = 0x6bff128c
    JETPACKPULLCMD = 0x321d6797
    JETPACKRECORDCMD = 0x44f35992
    JETPACKPREPARE = 0x4cd99cd1
    JETPACKACCEPT = 0x3e594d10
    JETPACKCOMMIT = 0x473d5af6
    JETPACKPULLRECSETINS = 0x542a2753
    JETPACKFINISHRECOVERY = 0x483dddaa

    __input_type_info__ = {
        'MsgString': ['std::string'],
        'MsgMarshall': ['MarshallDeputy'],
        'ReElect': [],
        'RuleSpeculativeExecute': ['MarshallDeputy'],
        'Dispatch': ['rrr::i64','DepId','MarshallDeputy'],
        'Prepare': ['rrr::i64','std::vector<rrr::i32>','DepId'],
        'Commit': ['rrr::i64','DepId'],
        'Abort': ['rrr::i64','DepId'],
        'EarlyAbort': ['rrr::i64'],
        'UpgradeEpoch': ['uint32_t'],
        'TruncateEpoch': ['uint32_t'],
        'IsLeader': ['locid_t'],
        'IsFPGALeader': ['locid_t'],
        'SimpleCmd': ['SimpleCommand'],
        'FailoverPauseSocketOut': [],
        'FailoverResumeSocketOut': [],
        'rpc_null': [],
        'TapirAccept': ['uint64_t','int64_t','int32_t'],
        'TapirFastAccept': ['uint64_t','std::vector<SimpleCommand>'],
        'TapirDecide': ['uint64_t','rrr::i32'],
        'CarouselReadAndPrepare': ['rrr::i64','MarshallDeputy','bool_t'],
        'CarouselAccept': ['uint64_t','int64_t','int32_t'],
        'CarouselFastAccept': ['uint64_t','std::vector<SimpleCommand>'],
        'CarouselDecide': ['uint64_t','rrr::i32'],
        'RccDispatch': ['std::vector<SimpleCommand>'],
        'RccFinish': ['cmdid_t','MarshallDeputy'],
        'RccInquire': ['txnid_t','int32_t'],
        'RccDispatchRo': ['SimpleCommand'],
        'RccInquireValidation': ['txid_t','int32_t'],
        'RccNotifyGlobalValidation': ['txid_t','int32_t','int32_t'],
        'JanusDispatch': ['std::vector<SimpleCommand>'],
        'RccCommit': ['cmdid_t','rank_t','int32_t','parent_set_t'],
        'JanusCommit': ['cmdid_t','rank_t','int32_t','MarshallDeputy'],
        'JanusCommitWoGraph': ['cmdid_t','rank_t','int32_t'],
        'JanusInquire': ['epoch_t','txnid_t'],
        'RccPreAccept': ['cmdid_t','rank_t','std::vector<SimpleCommand>'],
        'JanusPreAccept': ['cmdid_t','rank_t','std::vector<SimpleCommand>','MarshallDeputy'],
        'JanusPreAcceptWoGraph': ['cmdid_t','rank_t','std::vector<SimpleCommand>'],
        'RccAccept': ['cmdid_t','rrr::i32','ballot_t','parent_set_t'],
        'JanusAccept': ['cmdid_t','rrr::i32','ballot_t','MarshallDeputy'],
        'PreAcceptFebruus': ['txid_t'],
        'AcceptFebruus': ['txid_t','ballot_t','uint64_t'],
        'CommitFebruus': ['txid_t','uint64_t'],
        'JetpackBeginRecovery': ['MarshallDeputy','MarshallDeputy','epoch_t'],
        'JetpackPullIdSet': ['epoch_t','epoch_t'],
        'JetpackPullCmd': ['epoch_t','epoch_t','MarshallDeputy'],
        'JetpackRecordCmd': ['epoch_t','epoch_t','int32_t','int32_t','MarshallDeputy'],
        'JetpackPrepare': ['epoch_t','epoch_t','ballot_t'],
        'JetpackAccept': ['epoch_t','epoch_t','ballot_t','int32_t','int32_t'],
        'JetpackCommit': ['epoch_t','epoch_t','int32_t','int32_t'],
        'JetpackPullRecSetIns': ['epoch_t','epoch_t','int32_t','int32_t'],
        'JetpackFinishRecovery': ['epoch_t'],
    }

    __output_type_info__ = {
        'MsgString': ['std::string'],
        'MsgMarshall': ['MarshallDeputy'],
        'ReElect': ['bool_t'],
        'RuleSpeculativeExecute': ['bool_t','int32_t','bool_t'],
        'Dispatch': ['rrr::i32','TxnOutput','uint64_t','MarshallDeputy'],
        'Prepare': ['rrr::i32','bool_t','uint64_t'],
        'Commit': ['rrr::i32','bool_t','uint64_t','Profiling','MarshallDeputy'],
        'Abort': ['rrr::i32','bool_t','uint64_t','Profiling','MarshallDeputy'],
        'EarlyAbort': ['rrr::i32'],
        'UpgradeEpoch': ['int32_t'],
        'TruncateEpoch': [],
        'IsLeader': ['bool_t'],
        'IsFPGALeader': ['bool_t'],
        'SimpleCmd': ['rrr::i32'],
        'FailoverPauseSocketOut': ['rrr::i32'],
        'FailoverResumeSocketOut': ['rrr::i32'],
        'rpc_null': [],
        'TapirAccept': [],
        'TapirFastAccept': ['rrr::i32'],
        'TapirDecide': [],
        'CarouselReadAndPrepare': ['rrr::i32','TxnOutput'],
        'CarouselAccept': [],
        'CarouselFastAccept': ['rrr::i32'],
        'CarouselDecide': [],
        'RccDispatch': ['rrr::i32','TxnOutput','MarshallDeputy'],
        'RccFinish': ['std::map<uint32_t, std::map<int32_t, Value>>'],
        'RccInquire': ['std::map<uint64_t, parent_set_t>'],
        'RccDispatchRo': ['std::map<rrr::i32, Value>'],
        'RccInquireValidation': ['int32_t'],
        'RccNotifyGlobalValidation': [],
        'JanusDispatch': ['rrr::i32','TxnOutput','MarshallDeputy'],
        'RccCommit': ['int32_t','TxnOutput'],
        'JanusCommit': ['int32_t','TxnOutput'],
        'JanusCommitWoGraph': ['int32_t','TxnOutput'],
        'JanusInquire': ['MarshallDeputy'],
        'RccPreAccept': ['rrr::i32','parent_set_t'],
        'JanusPreAccept': ['rrr::i32','MarshallDeputy'],
        'JanusPreAcceptWoGraph': ['rrr::i32','MarshallDeputy'],
        'RccAccept': ['rrr::i32'],
        'JanusAccept': ['rrr::i32'],
        'PreAcceptFebruus': ['rrr::i32','uint64_t'],
        'AcceptFebruus': ['rrr::i32'],
        'CommitFebruus': ['rrr::i32'],
        'JetpackBeginRecovery': [],
        'JetpackPullIdSet': ['bool_t','epoch_t','epoch_t','MarshallDeputy','MarshallDeputy','MarshallDeputy'],
        'JetpackPullCmd': ['bool_t','epoch_t','epoch_t','MarshallDeputy','MarshallDeputy','MarshallDeputy'],
        'JetpackRecordCmd': [],
        'JetpackPrepare': ['bool_t','epoch_t','epoch_t','MarshallDeputy','MarshallDeputy','ballot_t','ballot_t','int32_t','int32_t'],
        'JetpackAccept': ['bool_t','epoch_t','epoch_t','MarshallDeputy','MarshallDeputy','ballot_t'],
        'JetpackCommit': [],
        'JetpackPullRecSetIns': ['bool_t','epoch_t','epoch_t','MarshallDeputy','MarshallDeputy','MarshallDeputy'],
        'JetpackFinishRecovery': [],
    }

    def __bind_helper__(self, func):
        def f(*args):
            return getattr(self, func.__name__)(*args)
        return f

    def __reg_to__(self, server):
        server.__reg_func__(ClassicService.MSGSTRING, self.__bind_helper__(self.MsgString), ['std::string'], ['std::string'])
        server.__reg_func__(ClassicService.MSGMARSHALL, self.__bind_helper__(self.MsgMarshall), ['MarshallDeputy'], ['MarshallDeputy'])
        server.__reg_func__(ClassicService.REELECT, self.__bind_helper__(self.ReElect), [], ['bool_t'])
        server.__reg_func__(ClassicService.RULESPECULATIVEEXECUTE, self.__bind_helper__(self.RuleSpeculativeExecute), ['MarshallDeputy'], ['bool_t','int32_t','bool_t'])
        server.__reg_func__(ClassicService.DISPATCH, self.__bind_helper__(self.Dispatch), ['rrr::i64','DepId','MarshallDeputy'], ['rrr::i32','TxnOutput','uint64_t','MarshallDeputy'])
        server.__reg_func__(ClassicService.PREPARE, self.__bind_helper__(self.Prepare), ['rrr::i64','std::vector<rrr::i32>','DepId'], ['rrr::i32','bool_t','uint64_t'])
        server.__reg_func__(ClassicService.COMMIT, self.__bind_helper__(self.Commit), ['rrr::i64','DepId'], ['rrr::i32','bool_t','uint64_t','Profiling','MarshallDeputy'])
        server.__reg_func__(ClassicService.ABORT, self.__bind_helper__(self.Abort), ['rrr::i64','DepId'], ['rrr::i32','bool_t','uint64_t','Profiling','MarshallDeputy'])
        server.__reg_func__(ClassicService.EARLYABORT, self.__bind_helper__(self.EarlyAbort), ['rrr::i64'], ['rrr::i32'])
        server.__reg_func__(ClassicService.UPGRADEEPOCH, self.__bind_helper__(self.UpgradeEpoch), ['uint32_t'], ['int32_t'])
        server.__reg_func__(ClassicService.TRUNCATEEPOCH, self.__bind_helper__(self.TruncateEpoch), ['uint32_t'], [])
        server.__reg_func__(ClassicService.ISLEADER, self.__bind_helper__(self.IsLeader), ['locid_t'], ['bool_t'])
        server.__reg_func__(ClassicService.ISFPGALEADER, self.__bind_helper__(self.IsFPGALeader), ['locid_t'], ['bool_t'])
        server.__reg_func__(ClassicService.SIMPLECMD, self.__bind_helper__(self.SimpleCmd), ['SimpleCommand'], ['rrr::i32'])
        server.__reg_func__(ClassicService.FAILOVERPAUSESOCKETOUT, self.__bind_helper__(self.FailoverPauseSocketOut), [], ['rrr::i32'])
        server.__reg_func__(ClassicService.FAILOVERRESUMESOCKETOUT, self.__bind_helper__(self.FailoverResumeSocketOut), [], ['rrr::i32'])
        server.__reg_func__(ClassicService.RPC_NULL, self.__bind_helper__(self.rpc_null), [], [])
        server.__reg_func__(ClassicService.TAPIRACCEPT, self.__bind_helper__(self.TapirAccept), ['uint64_t','int64_t','int32_t'], [])
        server.__reg_func__(ClassicService.TAPIRFASTACCEPT, self.__bind_helper__(self.TapirFastAccept), ['uint64_t','std::vector<SimpleCommand>'], ['rrr::i32'])
        server.__reg_func__(ClassicService.TAPIRDECIDE, self.__bind_helper__(self.TapirDecide), ['uint64_t','rrr::i32'], [])
        server.__reg_func__(ClassicService.CAROUSELREADANDPREPARE, self.__bind_helper__(self.CarouselReadAndPrepare), ['rrr::i64','MarshallDeputy','bool_t'], ['rrr::i32','TxnOutput'])
        server.__reg_func__(ClassicService.CAROUSELACCEPT, self.__bind_helper__(self.CarouselAccept), ['uint64_t','int64_t','int32_t'], [])
        server.__reg_func__(ClassicService.CAROUSELFASTACCEPT, self.__bind_helper__(self.CarouselFastAccept), ['uint64_t','std::vector<SimpleCommand>'], ['rrr::i32'])
        server.__reg_func__(ClassicService.CAROUSELDECIDE, self.__bind_helper__(self.CarouselDecide), ['uint64_t','rrr::i32'], [])
        server.__reg_func__(ClassicService.RCCDISPATCH, self.__bind_helper__(self.RccDispatch), ['std::vector<SimpleCommand>'], ['rrr::i32','TxnOutput','MarshallDeputy'])
        server.__reg_func__(ClassicService.RCCFINISH, self.__bind_helper__(self.RccFinish), ['cmdid_t','MarshallDeputy'], ['std::map<uint32_t, std::map<int32_t, Value>>'])
        server.__reg_func__(ClassicService.RCCINQUIRE, self.__bind_helper__(self.RccInquire), ['txnid_t','int32_t'], ['std::map<uint64_t, parent_set_t>'])
        server.__reg_func__(ClassicService.RCCDISPATCHRO, self.__bind_helper__(self.RccDispatchRo), ['SimpleCommand'], ['std::map<rrr::i32, Value>'])
        server.__reg_func__(ClassicService.RCCINQUIREVALIDATION, self.__bind_helper__(self.RccInquireValidation), ['txid_t','int32_t'], ['int32_t'])
        server.__reg_func__(ClassicService.RCCNOTIFYGLOBALVALIDATION, self.__bind_helper__(self.RccNotifyGlobalValidation), ['txid_t','int32_t','int32_t'], [])
        server.__reg_func__(ClassicService.JANUSDISPATCH, self.__bind_helper__(self.JanusDispatch), ['std::vector<SimpleCommand>'], ['rrr::i32','TxnOutput','MarshallDeputy'])
        server.__reg_func__(ClassicService.RCCCOMMIT, self.__bind_helper__(self.RccCommit), ['cmdid_t','rank_t','int32_t','parent_set_t'], ['int32_t','TxnOutput'])
        server.__reg_func__(ClassicService.JANUSCOMMIT, self.__bind_helper__(self.JanusCommit), ['cmdid_t','rank_t','int32_t','MarshallDeputy'], ['int32_t','TxnOutput'])
        server.__reg_func__(ClassicService.JANUSCOMMITWOGRAPH, self.__bind_helper__(self.JanusCommitWoGraph), ['cmdid_t','rank_t','int32_t'], ['int32_t','TxnOutput'])
        server.__reg_func__(ClassicService.JANUSINQUIRE, self.__bind_helper__(self.JanusInquire), ['epoch_t','txnid_t'], ['MarshallDeputy'])
        server.__reg_func__(ClassicService.RCCPREACCEPT, self.__bind_helper__(self.RccPreAccept), ['cmdid_t','rank_t','std::vector<SimpleCommand>'], ['rrr::i32','parent_set_t'])
        server.__reg_func__(ClassicService.JANUSPREACCEPT, self.__bind_helper__(self.JanusPreAccept), ['cmdid_t','rank_t','std::vector<SimpleCommand>','MarshallDeputy'], ['rrr::i32','MarshallDeputy'])
        server.__reg_func__(ClassicService.JANUSPREACCEPTWOGRAPH, self.__bind_helper__(self.JanusPreAcceptWoGraph), ['cmdid_t','rank_t','std::vector<SimpleCommand>'], ['rrr::i32','MarshallDeputy'])
        server.__reg_func__(ClassicService.RCCACCEPT, self.__bind_helper__(self.RccAccept), ['cmdid_t','rrr::i32','ballot_t','parent_set_t'], ['rrr::i32'])
        server.__reg_func__(ClassicService.JANUSACCEPT, self.__bind_helper__(self.JanusAccept), ['cmdid_t','rrr::i32','ballot_t','MarshallDeputy'], ['rrr::i32'])
        server.__reg_func__(ClassicService.PREACCEPTFEBRUUS, self.__bind_helper__(self.PreAcceptFebruus), ['txid_t'], ['rrr::i32','uint64_t'])
        server.__reg_func__(ClassicService.ACCEPTFEBRUUS, self.__bind_helper__(self.AcceptFebruus), ['txid_t','ballot_t','uint64_t'], ['rrr::i32'])
        server.__reg_func__(ClassicService.COMMITFEBRUUS, self.__bind_helper__(self.CommitFebruus), ['txid_t','uint64_t'], ['rrr::i32'])
        server.__reg_func__(ClassicService.JETPACKBEGINRECOVERY, self.__bind_helper__(self.JetpackBeginRecovery), ['MarshallDeputy','MarshallDeputy','epoch_t'], [])
        server.__reg_func__(ClassicService.JETPACKPULLIDSET, self.__bind_helper__(self.JetpackPullIdSet), ['epoch_t','epoch_t'], ['bool_t','epoch_t','epoch_t','MarshallDeputy','MarshallDeputy','MarshallDeputy'])
        server.__reg_func__(ClassicService.JETPACKPULLCMD, self.__bind_helper__(self.JetpackPullCmd), ['epoch_t','epoch_t','MarshallDeputy'], ['bool_t','epoch_t','epoch_t','MarshallDeputy','MarshallDeputy','MarshallDeputy'])
        server.__reg_func__(ClassicService.JETPACKRECORDCMD, self.__bind_helper__(self.JetpackRecordCmd), ['epoch_t','epoch_t','int32_t','int32_t','MarshallDeputy'], [])
        server.__reg_func__(ClassicService.JETPACKPREPARE, self.__bind_helper__(self.JetpackPrepare), ['epoch_t','epoch_t','ballot_t'], ['bool_t','epoch_t','epoch_t','MarshallDeputy','MarshallDeputy','ballot_t','ballot_t','int32_t','int32_t'])
        server.__reg_func__(ClassicService.JETPACKACCEPT, self.__bind_helper__(self.JetpackAccept), ['epoch_t','epoch_t','ballot_t','int32_t','int32_t'], ['bool_t','epoch_t','epoch_t','MarshallDeputy','MarshallDeputy','ballot_t'])
        server.__reg_func__(ClassicService.JETPACKCOMMIT, self.__bind_helper__(self.JetpackCommit), ['epoch_t','epoch_t','int32_t','int32_t'], [])
        server.__reg_func__(ClassicService.JETPACKPULLRECSETINS, self.__bind_helper__(self.JetpackPullRecSetIns), ['epoch_t','epoch_t','int32_t','int32_t'], ['bool_t','epoch_t','epoch_t','MarshallDeputy','MarshallDeputy','MarshallDeputy'])
        server.__reg_func__(ClassicService.JETPACKFINISHRECOVERY, self.__bind_helper__(self.JetpackFinishRecovery), ['epoch_t'], [])

    def MsgString(__self__, arg):
        raise NotImplementedError('subclass ClassicService and implement your own MsgString function')

    def MsgMarshall(__self__, arg):
        raise NotImplementedError('subclass ClassicService and implement your own MsgMarshall function')

    def ReElect(__self__):
        raise NotImplementedError('subclass ClassicService and implement your own ReElect function')

    def RuleSpeculativeExecute(__self__, md):
        raise NotImplementedError('subclass ClassicService and implement your own RuleSpeculativeExecute function')

    def Dispatch(__self__, tid, dep_id, cmd):
        raise NotImplementedError('subclass ClassicService and implement your own Dispatch function')

    def Prepare(__self__, tid, sids, dep_id):
        raise NotImplementedError('subclass ClassicService and implement your own Prepare function')

    def Commit(__self__, tid, dep_id):
        raise NotImplementedError('subclass ClassicService and implement your own Commit function')

    def Abort(__self__, tid, dep_id):
        raise NotImplementedError('subclass ClassicService and implement your own Abort function')

    def EarlyAbort(__self__, tid):
        raise NotImplementedError('subclass ClassicService and implement your own EarlyAbort function')

    def UpgradeEpoch(__self__, curr_epoch):
        raise NotImplementedError('subclass ClassicService and implement your own UpgradeEpoch function')

    def TruncateEpoch(__self__, old_epoch):
        raise NotImplementedError('subclass ClassicService and implement your own TruncateEpoch function')

    def IsLeader(__self__, cur_pause):
        raise NotImplementedError('subclass ClassicService and implement your own IsLeader function')

    def IsFPGALeader(__self__, cur_pause):
        raise NotImplementedError('subclass ClassicService and implement your own IsFPGALeader function')

    def SimpleCmd(__self__, cmd):
        raise NotImplementedError('subclass ClassicService and implement your own SimpleCmd function')

    def FailoverPauseSocketOut(__self__):
        raise NotImplementedError('subclass ClassicService and implement your own FailoverPauseSocketOut function')

    def FailoverResumeSocketOut(__self__):
        raise NotImplementedError('subclass ClassicService and implement your own FailoverResumeSocketOut function')

    def rpc_null(__self__):
        raise NotImplementedError('subclass ClassicService and implement your own rpc_null function')

    def TapirAccept(__self__, cmd_id, ballot, decision):
        raise NotImplementedError('subclass ClassicService and implement your own TapirAccept function')

    def TapirFastAccept(__self__, cmd_id, txn_cmds):
        raise NotImplementedError('subclass ClassicService and implement your own TapirFastAccept function')

    def TapirDecide(__self__, cmd_id, commit):
        raise NotImplementedError('subclass ClassicService and implement your own TapirDecide function')

    def CarouselReadAndPrepare(__self__, tid, cmd, leader):
        raise NotImplementedError('subclass ClassicService and implement your own CarouselReadAndPrepare function')

    def CarouselAccept(__self__, cmd_id, ballot, decision):
        raise NotImplementedError('subclass ClassicService and implement your own CarouselAccept function')

    def CarouselFastAccept(__self__, cmd_id, txn_cmds):
        raise NotImplementedError('subclass ClassicService and implement your own CarouselFastAccept function')

    def CarouselDecide(__self__, cmd_id, commit):
        raise NotImplementedError('subclass ClassicService and implement your own CarouselDecide function')

    def RccDispatch(__self__, cmd):
        raise NotImplementedError('subclass ClassicService and implement your own RccDispatch function')

    def RccFinish(__self__, id, md_graph):
        raise NotImplementedError('subclass ClassicService and implement your own RccFinish function')

    def RccInquire(__self__, txn_id, rank):
        raise NotImplementedError('subclass ClassicService and implement your own RccInquire function')

    def RccDispatchRo(__self__, cmd):
        raise NotImplementedError('subclass ClassicService and implement your own RccDispatchRo function')

    def RccInquireValidation(__self__, tx_id, rank):
        raise NotImplementedError('subclass ClassicService and implement your own RccInquireValidation function')

    def RccNotifyGlobalValidation(__self__, tx_id, rank, res):
        raise NotImplementedError('subclass ClassicService and implement your own RccNotifyGlobalValidation function')

    def JanusDispatch(__self__, cmd):
        raise NotImplementedError('subclass ClassicService and implement your own JanusDispatch function')

    def RccCommit(__self__, id, rank, need_validation, parents):
        raise NotImplementedError('subclass ClassicService and implement your own RccCommit function')

    def JanusCommit(__self__, id, rank, need_validation, graph):
        raise NotImplementedError('subclass ClassicService and implement your own JanusCommit function')

    def JanusCommitWoGraph(__self__, id, rank, need_validation):
        raise NotImplementedError('subclass ClassicService and implement your own JanusCommitWoGraph function')

    def JanusInquire(__self__, epoch, txn_id):
        raise NotImplementedError('subclass ClassicService and implement your own JanusInquire function')

    def RccPreAccept(__self__, txn_id, rank, cmd):
        raise NotImplementedError('subclass ClassicService and implement your own RccPreAccept function')

    def JanusPreAccept(__self__, txn_id, rank, cmd, graph):
        raise NotImplementedError('subclass ClassicService and implement your own JanusPreAccept function')

    def JanusPreAcceptWoGraph(__self__, txn_id, rank, cmd):
        raise NotImplementedError('subclass ClassicService and implement your own JanusPreAcceptWoGraph function')

    def RccAccept(__self__, txn_id, rank, ballot, p):
        raise NotImplementedError('subclass ClassicService and implement your own RccAccept function')

    def JanusAccept(__self__, txn_id, rank, ballot, graph):
        raise NotImplementedError('subclass ClassicService and implement your own JanusAccept function')

    def PreAcceptFebruus(__self__, tx_id):
        raise NotImplementedError('subclass ClassicService and implement your own PreAcceptFebruus function')

    def AcceptFebruus(__self__, tx_id, ballot, timestamp):
        raise NotImplementedError('subclass ClassicService and implement your own AcceptFebruus function')

    def CommitFebruus(__self__, tx_id, timestamp):
        raise NotImplementedError('subclass ClassicService and implement your own CommitFebruus function')

    def JetpackBeginRecovery(__self__, old_view, new_view, new_view_id):
        raise NotImplementedError('subclass ClassicService and implement your own JetpackBeginRecovery function')

    def JetpackPullIdSet(__self__, jepoch, oepoch):
        raise NotImplementedError('subclass ClassicService and implement your own JetpackPullIdSet function')

    def JetpackPullCmd(__self__, jepoch, oepoch, key_batch):
        raise NotImplementedError('subclass ClassicService and implement your own JetpackPullCmd function')

    def JetpackRecordCmd(__self__, jepoch, oepoch, sid, rid, cmd_batch):
        raise NotImplementedError('subclass ClassicService and implement your own JetpackRecordCmd function')

    def JetpackPrepare(__self__, jepoch, oepoch, max_seen_ballot):
        raise NotImplementedError('subclass ClassicService and implement your own JetpackPrepare function')

    def JetpackAccept(__self__, jepoch, oepoch, max_seen_ballot, sid, set_size):
        raise NotImplementedError('subclass ClassicService and implement your own JetpackAccept function')

    def JetpackCommit(__self__, jepoch, oepoch, sid, set_size):
        raise NotImplementedError('subclass ClassicService and implement your own JetpackCommit function')

    def JetpackPullRecSetIns(__self__, jepoch, oepoch, sid, rid):
        raise NotImplementedError('subclass ClassicService and implement your own JetpackPullRecSetIns function')

    def JetpackFinishRecovery(__self__, oepoch):
        raise NotImplementedError('subclass ClassicService and implement your own JetpackFinishRecovery function')

class ClassicProxy(object):
    def __init__(self, clnt):
        self.__clnt__ = clnt

    def async_MsgString(__self__, arg):
        return __self__.__clnt__.async_call(ClassicService.MSGSTRING, [arg], ClassicService.__input_type_info__['MsgString'], ClassicService.__output_type_info__['MsgString'])

    def async_MsgMarshall(__self__, arg):
        return __self__.__clnt__.async_call(ClassicService.MSGMARSHALL, [arg], ClassicService.__input_type_info__['MsgMarshall'], ClassicService.__output_type_info__['MsgMarshall'])

    def async_ReElect(__self__):
        return __self__.__clnt__.async_call(ClassicService.REELECT, [], ClassicService.__input_type_info__['ReElect'], ClassicService.__output_type_info__['ReElect'])

    def async_RuleSpeculativeExecute(__self__, md):
        return __self__.__clnt__.async_call(ClassicService.RULESPECULATIVEEXECUTE, [md], ClassicService.__input_type_info__['RuleSpeculativeExecute'], ClassicService.__output_type_info__['RuleSpeculativeExecute'])

    def async_Dispatch(__self__, tid, dep_id, cmd):
        return __self__.__clnt__.async_call(ClassicService.DISPATCH, [tid, dep_id, cmd], ClassicService.__input_type_info__['Dispatch'], ClassicService.__output_type_info__['Dispatch'])

    def async_Prepare(__self__, tid, sids, dep_id):
        return __self__.__clnt__.async_call(ClassicService.PREPARE, [tid, sids, dep_id], ClassicService.__input_type_info__['Prepare'], ClassicService.__output_type_info__['Prepare'])

    def async_Commit(__self__, tid, dep_id):
        return __self__.__clnt__.async_call(ClassicService.COMMIT, [tid, dep_id], ClassicService.__input_type_info__['Commit'], ClassicService.__output_type_info__['Commit'])

    def async_Abort(__self__, tid, dep_id):
        return __self__.__clnt__.async_call(ClassicService.ABORT, [tid, dep_id], ClassicService.__input_type_info__['Abort'], ClassicService.__output_type_info__['Abort'])

    def async_EarlyAbort(__self__, tid):
        return __self__.__clnt__.async_call(ClassicService.EARLYABORT, [tid], ClassicService.__input_type_info__['EarlyAbort'], ClassicService.__output_type_info__['EarlyAbort'])

    def async_UpgradeEpoch(__self__, curr_epoch):
        return __self__.__clnt__.async_call(ClassicService.UPGRADEEPOCH, [curr_epoch], ClassicService.__input_type_info__['UpgradeEpoch'], ClassicService.__output_type_info__['UpgradeEpoch'])

    def async_TruncateEpoch(__self__, old_epoch):
        return __self__.__clnt__.async_call(ClassicService.TRUNCATEEPOCH, [old_epoch], ClassicService.__input_type_info__['TruncateEpoch'], ClassicService.__output_type_info__['TruncateEpoch'])

    def async_IsLeader(__self__, cur_pause):
        return __self__.__clnt__.async_call(ClassicService.ISLEADER, [cur_pause], ClassicService.__input_type_info__['IsLeader'], ClassicService.__output_type_info__['IsLeader'])

    def async_IsFPGALeader(__self__, cur_pause):
        return __self__.__clnt__.async_call(ClassicService.ISFPGALEADER, [cur_pause], ClassicService.__input_type_info__['IsFPGALeader'], ClassicService.__output_type_info__['IsFPGALeader'])

    def async_SimpleCmd(__self__, cmd):
        return __self__.__clnt__.async_call(ClassicService.SIMPLECMD, [cmd], ClassicService.__input_type_info__['SimpleCmd'], ClassicService.__output_type_info__['SimpleCmd'])

    def async_FailoverPauseSocketOut(__self__):
        return __self__.__clnt__.async_call(ClassicService.FAILOVERPAUSESOCKETOUT, [], ClassicService.__input_type_info__['FailoverPauseSocketOut'], ClassicService.__output_type_info__['FailoverPauseSocketOut'])

    def async_FailoverResumeSocketOut(__self__):
        return __self__.__clnt__.async_call(ClassicService.FAILOVERRESUMESOCKETOUT, [], ClassicService.__input_type_info__['FailoverResumeSocketOut'], ClassicService.__output_type_info__['FailoverResumeSocketOut'])

    def async_rpc_null(__self__):
        return __self__.__clnt__.async_call(ClassicService.RPC_NULL, [], ClassicService.__input_type_info__['rpc_null'], ClassicService.__output_type_info__['rpc_null'])

    def async_TapirAccept(__self__, cmd_id, ballot, decision):
        return __self__.__clnt__.async_call(ClassicService.TAPIRACCEPT, [cmd_id, ballot, decision], ClassicService.__input_type_info__['TapirAccept'], ClassicService.__output_type_info__['TapirAccept'])

    def async_TapirFastAccept(__self__, cmd_id, txn_cmds):
        return __self__.__clnt__.async_call(ClassicService.TAPIRFASTACCEPT, [cmd_id, txn_cmds], ClassicService.__input_type_info__['TapirFastAccept'], ClassicService.__output_type_info__['TapirFastAccept'])

    def async_TapirDecide(__self__, cmd_id, commit):
        return __self__.__clnt__.async_call(ClassicService.TAPIRDECIDE, [cmd_id, commit], ClassicService.__input_type_info__['TapirDecide'], ClassicService.__output_type_info__['TapirDecide'])

    def async_CarouselReadAndPrepare(__self__, tid, cmd, leader):
        return __self__.__clnt__.async_call(ClassicService.CAROUSELREADANDPREPARE, [tid, cmd, leader], ClassicService.__input_type_info__['CarouselReadAndPrepare'], ClassicService.__output_type_info__['CarouselReadAndPrepare'])

    def async_CarouselAccept(__self__, cmd_id, ballot, decision):
        return __self__.__clnt__.async_call(ClassicService.CAROUSELACCEPT, [cmd_id, ballot, decision], ClassicService.__input_type_info__['CarouselAccept'], ClassicService.__output_type_info__['CarouselAccept'])

    def async_CarouselFastAccept(__self__, cmd_id, txn_cmds):
        return __self__.__clnt__.async_call(ClassicService.CAROUSELFASTACCEPT, [cmd_id, txn_cmds], ClassicService.__input_type_info__['CarouselFastAccept'], ClassicService.__output_type_info__['CarouselFastAccept'])

    def async_CarouselDecide(__self__, cmd_id, commit):
        return __self__.__clnt__.async_call(ClassicService.CAROUSELDECIDE, [cmd_id, commit], ClassicService.__input_type_info__['CarouselDecide'], ClassicService.__output_type_info__['CarouselDecide'])

    def async_RccDispatch(__self__, cmd):
        return __self__.__clnt__.async_call(ClassicService.RCCDISPATCH, [cmd], ClassicService.__input_type_info__['RccDispatch'], ClassicService.__output_type_info__['RccDispatch'])

    def async_RccFinish(__self__, id, md_graph):
        return __self__.__clnt__.async_call(ClassicService.RCCFINISH, [id, md_graph], ClassicService.__input_type_info__['RccFinish'], ClassicService.__output_type_info__['RccFinish'])

    def async_RccInquire(__self__, txn_id, rank):
        return __self__.__clnt__.async_call(ClassicService.RCCINQUIRE, [txn_id, rank], ClassicService.__input_type_info__['RccInquire'], ClassicService.__output_type_info__['RccInquire'])

    def async_RccDispatchRo(__self__, cmd):
        return __self__.__clnt__.async_call(ClassicService.RCCDISPATCHRO, [cmd], ClassicService.__input_type_info__['RccDispatchRo'], ClassicService.__output_type_info__['RccDispatchRo'])

    def async_RccInquireValidation(__self__, tx_id, rank):
        return __self__.__clnt__.async_call(ClassicService.RCCINQUIREVALIDATION, [tx_id, rank], ClassicService.__input_type_info__['RccInquireValidation'], ClassicService.__output_type_info__['RccInquireValidation'])

    def async_RccNotifyGlobalValidation(__self__, tx_id, rank, res):
        return __self__.__clnt__.async_call(ClassicService.RCCNOTIFYGLOBALVALIDATION, [tx_id, rank, res], ClassicService.__input_type_info__['RccNotifyGlobalValidation'], ClassicService.__output_type_info__['RccNotifyGlobalValidation'])

    def async_JanusDispatch(__self__, cmd):
        return __self__.__clnt__.async_call(ClassicService.JANUSDISPATCH, [cmd], ClassicService.__input_type_info__['JanusDispatch'], ClassicService.__output_type_info__['JanusDispatch'])

    def async_RccCommit(__self__, id, rank, need_validation, parents):
        return __self__.__clnt__.async_call(ClassicService.RCCCOMMIT, [id, rank, need_validation, parents], ClassicService.__input_type_info__['RccCommit'], ClassicService.__output_type_info__['RccCommit'])

    def async_JanusCommit(__self__, id, rank, need_validation, graph):
        return __self__.__clnt__.async_call(ClassicService.JANUSCOMMIT, [id, rank, need_validation, graph], ClassicService.__input_type_info__['JanusCommit'], ClassicService.__output_type_info__['JanusCommit'])

    def async_JanusCommitWoGraph(__self__, id, rank, need_validation):
        return __self__.__clnt__.async_call(ClassicService.JANUSCOMMITWOGRAPH, [id, rank, need_validation], ClassicService.__input_type_info__['JanusCommitWoGraph'], ClassicService.__output_type_info__['JanusCommitWoGraph'])

    def async_JanusInquire(__self__, epoch, txn_id):
        return __self__.__clnt__.async_call(ClassicService.JANUSINQUIRE, [epoch, txn_id], ClassicService.__input_type_info__['JanusInquire'], ClassicService.__output_type_info__['JanusInquire'])

    def async_RccPreAccept(__self__, txn_id, rank, cmd):
        return __self__.__clnt__.async_call(ClassicService.RCCPREACCEPT, [txn_id, rank, cmd], ClassicService.__input_type_info__['RccPreAccept'], ClassicService.__output_type_info__['RccPreAccept'])

    def async_JanusPreAccept(__self__, txn_id, rank, cmd, graph):
        return __self__.__clnt__.async_call(ClassicService.JANUSPREACCEPT, [txn_id, rank, cmd, graph], ClassicService.__input_type_info__['JanusPreAccept'], ClassicService.__output_type_info__['JanusPreAccept'])

    def async_JanusPreAcceptWoGraph(__self__, txn_id, rank, cmd):
        return __self__.__clnt__.async_call(ClassicService.JANUSPREACCEPTWOGRAPH, [txn_id, rank, cmd], ClassicService.__input_type_info__['JanusPreAcceptWoGraph'], ClassicService.__output_type_info__['JanusPreAcceptWoGraph'])

    def async_RccAccept(__self__, txn_id, rank, ballot, p):
        return __self__.__clnt__.async_call(ClassicService.RCCACCEPT, [txn_id, rank, ballot, p], ClassicService.__input_type_info__['RccAccept'], ClassicService.__output_type_info__['RccAccept'])

    def async_JanusAccept(__self__, txn_id, rank, ballot, graph):
        return __self__.__clnt__.async_call(ClassicService.JANUSACCEPT, [txn_id, rank, ballot, graph], ClassicService.__input_type_info__['JanusAccept'], ClassicService.__output_type_info__['JanusAccept'])

    def async_PreAcceptFebruus(__self__, tx_id):
        return __self__.__clnt__.async_call(ClassicService.PREACCEPTFEBRUUS, [tx_id], ClassicService.__input_type_info__['PreAcceptFebruus'], ClassicService.__output_type_info__['PreAcceptFebruus'])

    def async_AcceptFebruus(__self__, tx_id, ballot, timestamp):
        return __self__.__clnt__.async_call(ClassicService.ACCEPTFEBRUUS, [tx_id, ballot, timestamp], ClassicService.__input_type_info__['AcceptFebruus'], ClassicService.__output_type_info__['AcceptFebruus'])

    def async_CommitFebruus(__self__, tx_id, timestamp):
        return __self__.__clnt__.async_call(ClassicService.COMMITFEBRUUS, [tx_id, timestamp], ClassicService.__input_type_info__['CommitFebruus'], ClassicService.__output_type_info__['CommitFebruus'])

    def async_JetpackBeginRecovery(__self__, old_view, new_view, new_view_id):
        return __self__.__clnt__.async_call(ClassicService.JETPACKBEGINRECOVERY, [old_view, new_view, new_view_id], ClassicService.__input_type_info__['JetpackBeginRecovery'], ClassicService.__output_type_info__['JetpackBeginRecovery'])

    def async_JetpackPullIdSet(__self__, jepoch, oepoch):
        return __self__.__clnt__.async_call(ClassicService.JETPACKPULLIDSET, [jepoch, oepoch], ClassicService.__input_type_info__['JetpackPullIdSet'], ClassicService.__output_type_info__['JetpackPullIdSet'])

    def async_JetpackPullCmd(__self__, jepoch, oepoch, key_batch):
        return __self__.__clnt__.async_call(ClassicService.JETPACKPULLCMD, [jepoch, oepoch, key_batch], ClassicService.__input_type_info__['JetpackPullCmd'], ClassicService.__output_type_info__['JetpackPullCmd'])

    def async_JetpackRecordCmd(__self__, jepoch, oepoch, sid, rid, cmd_batch):
        return __self__.__clnt__.async_call(ClassicService.JETPACKRECORDCMD, [jepoch, oepoch, sid, rid, cmd_batch], ClassicService.__input_type_info__['JetpackRecordCmd'], ClassicService.__output_type_info__['JetpackRecordCmd'])

    def async_JetpackPrepare(__self__, jepoch, oepoch, max_seen_ballot):
        return __self__.__clnt__.async_call(ClassicService.JETPACKPREPARE, [jepoch, oepoch, max_seen_ballot], ClassicService.__input_type_info__['JetpackPrepare'], ClassicService.__output_type_info__['JetpackPrepare'])

    def async_JetpackAccept(__self__, jepoch, oepoch, max_seen_ballot, sid, set_size):
        return __self__.__clnt__.async_call(ClassicService.JETPACKACCEPT, [jepoch, oepoch, max_seen_ballot, sid, set_size], ClassicService.__input_type_info__['JetpackAccept'], ClassicService.__output_type_info__['JetpackAccept'])

    def async_JetpackCommit(__self__, jepoch, oepoch, sid, set_size):
        return __self__.__clnt__.async_call(ClassicService.JETPACKCOMMIT, [jepoch, oepoch, sid, set_size], ClassicService.__input_type_info__['JetpackCommit'], ClassicService.__output_type_info__['JetpackCommit'])

    def async_JetpackPullRecSetIns(__self__, jepoch, oepoch, sid, rid):
        return __self__.__clnt__.async_call(ClassicService.JETPACKPULLRECSETINS, [jepoch, oepoch, sid, rid], ClassicService.__input_type_info__['JetpackPullRecSetIns'], ClassicService.__output_type_info__['JetpackPullRecSetIns'])

    def async_JetpackFinishRecovery(__self__, oepoch):
        return __self__.__clnt__.async_call(ClassicService.JETPACKFINISHRECOVERY, [oepoch], ClassicService.__input_type_info__['JetpackFinishRecovery'], ClassicService.__output_type_info__['JetpackFinishRecovery'])

    def sync_MsgString(__self__, arg):
        __result__ = __self__.__clnt__.sync_call(ClassicService.MSGSTRING, [arg], ClassicService.__input_type_info__['MsgString'], ClassicService.__output_type_info__['MsgString'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_MsgMarshall(__self__, arg):
        __result__ = __self__.__clnt__.sync_call(ClassicService.MSGMARSHALL, [arg], ClassicService.__input_type_info__['MsgMarshall'], ClassicService.__output_type_info__['MsgMarshall'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_ReElect(__self__):
        __result__ = __self__.__clnt__.sync_call(ClassicService.REELECT, [], ClassicService.__input_type_info__['ReElect'], ClassicService.__output_type_info__['ReElect'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_RuleSpeculativeExecute(__self__, md):
        __result__ = __self__.__clnt__.sync_call(ClassicService.RULESPECULATIVEEXECUTE, [md], ClassicService.__input_type_info__['RuleSpeculativeExecute'], ClassicService.__output_type_info__['RuleSpeculativeExecute'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_Dispatch(__self__, tid, dep_id, cmd):
        __result__ = __self__.__clnt__.sync_call(ClassicService.DISPATCH, [tid, dep_id, cmd], ClassicService.__input_type_info__['Dispatch'], ClassicService.__output_type_info__['Dispatch'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_Prepare(__self__, tid, sids, dep_id):
        __result__ = __self__.__clnt__.sync_call(ClassicService.PREPARE, [tid, sids, dep_id], ClassicService.__input_type_info__['Prepare'], ClassicService.__output_type_info__['Prepare'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_Commit(__self__, tid, dep_id):
        __result__ = __self__.__clnt__.sync_call(ClassicService.COMMIT, [tid, dep_id], ClassicService.__input_type_info__['Commit'], ClassicService.__output_type_info__['Commit'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_Abort(__self__, tid, dep_id):
        __result__ = __self__.__clnt__.sync_call(ClassicService.ABORT, [tid, dep_id], ClassicService.__input_type_info__['Abort'], ClassicService.__output_type_info__['Abort'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_EarlyAbort(__self__, tid):
        __result__ = __self__.__clnt__.sync_call(ClassicService.EARLYABORT, [tid], ClassicService.__input_type_info__['EarlyAbort'], ClassicService.__output_type_info__['EarlyAbort'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_UpgradeEpoch(__self__, curr_epoch):
        __result__ = __self__.__clnt__.sync_call(ClassicService.UPGRADEEPOCH, [curr_epoch], ClassicService.__input_type_info__['UpgradeEpoch'], ClassicService.__output_type_info__['UpgradeEpoch'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_TruncateEpoch(__self__, old_epoch):
        __result__ = __self__.__clnt__.sync_call(ClassicService.TRUNCATEEPOCH, [old_epoch], ClassicService.__input_type_info__['TruncateEpoch'], ClassicService.__output_type_info__['TruncateEpoch'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_IsLeader(__self__, cur_pause):
        __result__ = __self__.__clnt__.sync_call(ClassicService.ISLEADER, [cur_pause], ClassicService.__input_type_info__['IsLeader'], ClassicService.__output_type_info__['IsLeader'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_IsFPGALeader(__self__, cur_pause):
        __result__ = __self__.__clnt__.sync_call(ClassicService.ISFPGALEADER, [cur_pause], ClassicService.__input_type_info__['IsFPGALeader'], ClassicService.__output_type_info__['IsFPGALeader'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_SimpleCmd(__self__, cmd):
        __result__ = __self__.__clnt__.sync_call(ClassicService.SIMPLECMD, [cmd], ClassicService.__input_type_info__['SimpleCmd'], ClassicService.__output_type_info__['SimpleCmd'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_FailoverPauseSocketOut(__self__):
        __result__ = __self__.__clnt__.sync_call(ClassicService.FAILOVERPAUSESOCKETOUT, [], ClassicService.__input_type_info__['FailoverPauseSocketOut'], ClassicService.__output_type_info__['FailoverPauseSocketOut'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_FailoverResumeSocketOut(__self__):
        __result__ = __self__.__clnt__.sync_call(ClassicService.FAILOVERRESUMESOCKETOUT, [], ClassicService.__input_type_info__['FailoverResumeSocketOut'], ClassicService.__output_type_info__['FailoverResumeSocketOut'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_rpc_null(__self__):
        __result__ = __self__.__clnt__.sync_call(ClassicService.RPC_NULL, [], ClassicService.__input_type_info__['rpc_null'], ClassicService.__output_type_info__['rpc_null'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_TapirAccept(__self__, cmd_id, ballot, decision):
        __result__ = __self__.__clnt__.sync_call(ClassicService.TAPIRACCEPT, [cmd_id, ballot, decision], ClassicService.__input_type_info__['TapirAccept'], ClassicService.__output_type_info__['TapirAccept'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_TapirFastAccept(__self__, cmd_id, txn_cmds):
        __result__ = __self__.__clnt__.sync_call(ClassicService.TAPIRFASTACCEPT, [cmd_id, txn_cmds], ClassicService.__input_type_info__['TapirFastAccept'], ClassicService.__output_type_info__['TapirFastAccept'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_TapirDecide(__self__, cmd_id, commit):
        __result__ = __self__.__clnt__.sync_call(ClassicService.TAPIRDECIDE, [cmd_id, commit], ClassicService.__input_type_info__['TapirDecide'], ClassicService.__output_type_info__['TapirDecide'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_CarouselReadAndPrepare(__self__, tid, cmd, leader):
        __result__ = __self__.__clnt__.sync_call(ClassicService.CAROUSELREADANDPREPARE, [tid, cmd, leader], ClassicService.__input_type_info__['CarouselReadAndPrepare'], ClassicService.__output_type_info__['CarouselReadAndPrepare'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_CarouselAccept(__self__, cmd_id, ballot, decision):
        __result__ = __self__.__clnt__.sync_call(ClassicService.CAROUSELACCEPT, [cmd_id, ballot, decision], ClassicService.__input_type_info__['CarouselAccept'], ClassicService.__output_type_info__['CarouselAccept'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_CarouselFastAccept(__self__, cmd_id, txn_cmds):
        __result__ = __self__.__clnt__.sync_call(ClassicService.CAROUSELFASTACCEPT, [cmd_id, txn_cmds], ClassicService.__input_type_info__['CarouselFastAccept'], ClassicService.__output_type_info__['CarouselFastAccept'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_CarouselDecide(__self__, cmd_id, commit):
        __result__ = __self__.__clnt__.sync_call(ClassicService.CAROUSELDECIDE, [cmd_id, commit], ClassicService.__input_type_info__['CarouselDecide'], ClassicService.__output_type_info__['CarouselDecide'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_RccDispatch(__self__, cmd):
        __result__ = __self__.__clnt__.sync_call(ClassicService.RCCDISPATCH, [cmd], ClassicService.__input_type_info__['RccDispatch'], ClassicService.__output_type_info__['RccDispatch'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_RccFinish(__self__, id, md_graph):
        __result__ = __self__.__clnt__.sync_call(ClassicService.RCCFINISH, [id, md_graph], ClassicService.__input_type_info__['RccFinish'], ClassicService.__output_type_info__['RccFinish'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_RccInquire(__self__, txn_id, rank):
        __result__ = __self__.__clnt__.sync_call(ClassicService.RCCINQUIRE, [txn_id, rank], ClassicService.__input_type_info__['RccInquire'], ClassicService.__output_type_info__['RccInquire'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_RccDispatchRo(__self__, cmd):
        __result__ = __self__.__clnt__.sync_call(ClassicService.RCCDISPATCHRO, [cmd], ClassicService.__input_type_info__['RccDispatchRo'], ClassicService.__output_type_info__['RccDispatchRo'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_RccInquireValidation(__self__, tx_id, rank):
        __result__ = __self__.__clnt__.sync_call(ClassicService.RCCINQUIREVALIDATION, [tx_id, rank], ClassicService.__input_type_info__['RccInquireValidation'], ClassicService.__output_type_info__['RccInquireValidation'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_RccNotifyGlobalValidation(__self__, tx_id, rank, res):
        __result__ = __self__.__clnt__.sync_call(ClassicService.RCCNOTIFYGLOBALVALIDATION, [tx_id, rank, res], ClassicService.__input_type_info__['RccNotifyGlobalValidation'], ClassicService.__output_type_info__['RccNotifyGlobalValidation'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_JanusDispatch(__self__, cmd):
        __result__ = __self__.__clnt__.sync_call(ClassicService.JANUSDISPATCH, [cmd], ClassicService.__input_type_info__['JanusDispatch'], ClassicService.__output_type_info__['JanusDispatch'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_RccCommit(__self__, id, rank, need_validation, parents):
        __result__ = __self__.__clnt__.sync_call(ClassicService.RCCCOMMIT, [id, rank, need_validation, parents], ClassicService.__input_type_info__['RccCommit'], ClassicService.__output_type_info__['RccCommit'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_JanusCommit(__self__, id, rank, need_validation, graph):
        __result__ = __self__.__clnt__.sync_call(ClassicService.JANUSCOMMIT, [id, rank, need_validation, graph], ClassicService.__input_type_info__['JanusCommit'], ClassicService.__output_type_info__['JanusCommit'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_JanusCommitWoGraph(__self__, id, rank, need_validation):
        __result__ = __self__.__clnt__.sync_call(ClassicService.JANUSCOMMITWOGRAPH, [id, rank, need_validation], ClassicService.__input_type_info__['JanusCommitWoGraph'], ClassicService.__output_type_info__['JanusCommitWoGraph'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_JanusInquire(__self__, epoch, txn_id):
        __result__ = __self__.__clnt__.sync_call(ClassicService.JANUSINQUIRE, [epoch, txn_id], ClassicService.__input_type_info__['JanusInquire'], ClassicService.__output_type_info__['JanusInquire'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_RccPreAccept(__self__, txn_id, rank, cmd):
        __result__ = __self__.__clnt__.sync_call(ClassicService.RCCPREACCEPT, [txn_id, rank, cmd], ClassicService.__input_type_info__['RccPreAccept'], ClassicService.__output_type_info__['RccPreAccept'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_JanusPreAccept(__self__, txn_id, rank, cmd, graph):
        __result__ = __self__.__clnt__.sync_call(ClassicService.JANUSPREACCEPT, [txn_id, rank, cmd, graph], ClassicService.__input_type_info__['JanusPreAccept'], ClassicService.__output_type_info__['JanusPreAccept'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_JanusPreAcceptWoGraph(__self__, txn_id, rank, cmd):
        __result__ = __self__.__clnt__.sync_call(ClassicService.JANUSPREACCEPTWOGRAPH, [txn_id, rank, cmd], ClassicService.__input_type_info__['JanusPreAcceptWoGraph'], ClassicService.__output_type_info__['JanusPreAcceptWoGraph'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_RccAccept(__self__, txn_id, rank, ballot, p):
        __result__ = __self__.__clnt__.sync_call(ClassicService.RCCACCEPT, [txn_id, rank, ballot, p], ClassicService.__input_type_info__['RccAccept'], ClassicService.__output_type_info__['RccAccept'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_JanusAccept(__self__, txn_id, rank, ballot, graph):
        __result__ = __self__.__clnt__.sync_call(ClassicService.JANUSACCEPT, [txn_id, rank, ballot, graph], ClassicService.__input_type_info__['JanusAccept'], ClassicService.__output_type_info__['JanusAccept'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_PreAcceptFebruus(__self__, tx_id):
        __result__ = __self__.__clnt__.sync_call(ClassicService.PREACCEPTFEBRUUS, [tx_id], ClassicService.__input_type_info__['PreAcceptFebruus'], ClassicService.__output_type_info__['PreAcceptFebruus'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_AcceptFebruus(__self__, tx_id, ballot, timestamp):
        __result__ = __self__.__clnt__.sync_call(ClassicService.ACCEPTFEBRUUS, [tx_id, ballot, timestamp], ClassicService.__input_type_info__['AcceptFebruus'], ClassicService.__output_type_info__['AcceptFebruus'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_CommitFebruus(__self__, tx_id, timestamp):
        __result__ = __self__.__clnt__.sync_call(ClassicService.COMMITFEBRUUS, [tx_id, timestamp], ClassicService.__input_type_info__['CommitFebruus'], ClassicService.__output_type_info__['CommitFebruus'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_JetpackBeginRecovery(__self__, old_view, new_view, new_view_id):
        __result__ = __self__.__clnt__.sync_call(ClassicService.JETPACKBEGINRECOVERY, [old_view, new_view, new_view_id], ClassicService.__input_type_info__['JetpackBeginRecovery'], ClassicService.__output_type_info__['JetpackBeginRecovery'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_JetpackPullIdSet(__self__, jepoch, oepoch):
        __result__ = __self__.__clnt__.sync_call(ClassicService.JETPACKPULLIDSET, [jepoch, oepoch], ClassicService.__input_type_info__['JetpackPullIdSet'], ClassicService.__output_type_info__['JetpackPullIdSet'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_JetpackPullCmd(__self__, jepoch, oepoch, key_batch):
        __result__ = __self__.__clnt__.sync_call(ClassicService.JETPACKPULLCMD, [jepoch, oepoch, key_batch], ClassicService.__input_type_info__['JetpackPullCmd'], ClassicService.__output_type_info__['JetpackPullCmd'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_JetpackRecordCmd(__self__, jepoch, oepoch, sid, rid, cmd_batch):
        __result__ = __self__.__clnt__.sync_call(ClassicService.JETPACKRECORDCMD, [jepoch, oepoch, sid, rid, cmd_batch], ClassicService.__input_type_info__['JetpackRecordCmd'], ClassicService.__output_type_info__['JetpackRecordCmd'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_JetpackPrepare(__self__, jepoch, oepoch, max_seen_ballot):
        __result__ = __self__.__clnt__.sync_call(ClassicService.JETPACKPREPARE, [jepoch, oepoch, max_seen_ballot], ClassicService.__input_type_info__['JetpackPrepare'], ClassicService.__output_type_info__['JetpackPrepare'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_JetpackAccept(__self__, jepoch, oepoch, max_seen_ballot, sid, set_size):
        __result__ = __self__.__clnt__.sync_call(ClassicService.JETPACKACCEPT, [jepoch, oepoch, max_seen_ballot, sid, set_size], ClassicService.__input_type_info__['JetpackAccept'], ClassicService.__output_type_info__['JetpackAccept'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_JetpackCommit(__self__, jepoch, oepoch, sid, set_size):
        __result__ = __self__.__clnt__.sync_call(ClassicService.JETPACKCOMMIT, [jepoch, oepoch, sid, set_size], ClassicService.__input_type_info__['JetpackCommit'], ClassicService.__output_type_info__['JetpackCommit'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_JetpackPullRecSetIns(__self__, jepoch, oepoch, sid, rid):
        __result__ = __self__.__clnt__.sync_call(ClassicService.JETPACKPULLRECSETINS, [jepoch, oepoch, sid, rid], ClassicService.__input_type_info__['JetpackPullRecSetIns'], ClassicService.__output_type_info__['JetpackPullRecSetIns'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_JetpackFinishRecovery(__self__, oepoch):
        __result__ = __self__.__clnt__.sync_call(ClassicService.JETPACKFINISHRECOVERY, [oepoch], ClassicService.__input_type_info__['JetpackFinishRecovery'], ClassicService.__output_type_info__['JetpackFinishRecovery'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

class ServerControlService(object):
    SERVER_SHUTDOWN = 0x490820d5
    SERVER_READY = 0x54061a48
    SERVER_HEART_BEAT_WITH_DATA = 0x3ed41b13
    SERVER_HEART_BEAT = 0x559e3e2e

    __input_type_info__ = {
        'server_shutdown': [],
        'server_ready': [],
        'server_heart_beat_with_data': [],
        'server_heart_beat': [],
    }

    __output_type_info__ = {
        'server_shutdown': [],
        'server_ready': ['rrr::i32'],
        'server_heart_beat_with_data': ['ServerResponse'],
        'server_heart_beat': [],
    }

    def __bind_helper__(self, func):
        def f(*args):
            return getattr(self, func.__name__)(*args)
        return f

    def __reg_to__(self, server):
        server.__reg_func__(ServerControlService.SERVER_SHUTDOWN, self.__bind_helper__(self.server_shutdown), [], [])
        server.__reg_func__(ServerControlService.SERVER_READY, self.__bind_helper__(self.server_ready), [], ['rrr::i32'])
        server.__reg_func__(ServerControlService.SERVER_HEART_BEAT_WITH_DATA, self.__bind_helper__(self.server_heart_beat_with_data), [], ['ServerResponse'])
        server.__reg_func__(ServerControlService.SERVER_HEART_BEAT, self.__bind_helper__(self.server_heart_beat), [], [])

    def server_shutdown(__self__):
        raise NotImplementedError('subclass ServerControlService and implement your own server_shutdown function')

    def server_ready(__self__):
        raise NotImplementedError('subclass ServerControlService and implement your own server_ready function')

    def server_heart_beat_with_data(__self__):
        raise NotImplementedError('subclass ServerControlService and implement your own server_heart_beat_with_data function')

    def server_heart_beat(__self__):
        raise NotImplementedError('subclass ServerControlService and implement your own server_heart_beat function')

class ServerControlProxy(object):
    def __init__(self, clnt):
        self.__clnt__ = clnt

    def async_server_shutdown(__self__):
        return __self__.__clnt__.async_call(ServerControlService.SERVER_SHUTDOWN, [], ServerControlService.__input_type_info__['server_shutdown'], ServerControlService.__output_type_info__['server_shutdown'])

    def async_server_ready(__self__):
        return __self__.__clnt__.async_call(ServerControlService.SERVER_READY, [], ServerControlService.__input_type_info__['server_ready'], ServerControlService.__output_type_info__['server_ready'])

    def async_server_heart_beat_with_data(__self__):
        return __self__.__clnt__.async_call(ServerControlService.SERVER_HEART_BEAT_WITH_DATA, [], ServerControlService.__input_type_info__['server_heart_beat_with_data'], ServerControlService.__output_type_info__['server_heart_beat_with_data'])

    def async_server_heart_beat(__self__):
        return __self__.__clnt__.async_call(ServerControlService.SERVER_HEART_BEAT, [], ServerControlService.__input_type_info__['server_heart_beat'], ServerControlService.__output_type_info__['server_heart_beat'])

    def sync_server_shutdown(__self__):
        __result__ = __self__.__clnt__.sync_call(ServerControlService.SERVER_SHUTDOWN, [], ServerControlService.__input_type_info__['server_shutdown'], ServerControlService.__output_type_info__['server_shutdown'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_server_ready(__self__):
        __result__ = __self__.__clnt__.sync_call(ServerControlService.SERVER_READY, [], ServerControlService.__input_type_info__['server_ready'], ServerControlService.__output_type_info__['server_ready'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_server_heart_beat_with_data(__self__):
        __result__ = __self__.__clnt__.sync_call(ServerControlService.SERVER_HEART_BEAT_WITH_DATA, [], ServerControlService.__input_type_info__['server_heart_beat_with_data'], ServerControlService.__output_type_info__['server_heart_beat_with_data'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_server_heart_beat(__self__):
        __result__ = __self__.__clnt__.sync_call(ServerControlService.SERVER_HEART_BEAT, [], ServerControlService.__input_type_info__['server_heart_beat'], ServerControlService.__output_type_info__['server_heart_beat'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

class ClientControlService(object):
    CLIENT_GET_TXN_NAMES = 0x21d44209
    CLIENT_SHUTDOWN = 0x69ff127f
    CLIENT_FORCE_STOP = 0x15576036
    CLIENT_RESPONSE = 0x4f71e284
    CLIENT_READY = 0x393fe5e9
    CLIENT_READY_BLOCK = 0x545c3fe1
    CLIENT_START = 0x2217800d
    DISPATCHTXN = 0x477f449a

    __input_type_info__ = {
        'client_get_txn_names': [],
        'client_shutdown': [],
        'client_force_stop': [],
        'client_response': ['DepId'],
        'client_ready': [],
        'client_ready_block': [],
        'client_start': [],
        'DispatchTxn': ['TxDispatchRequest'],
    }

    __output_type_info__ = {
        'client_get_txn_names': ['std::map<rrr::i32, std::string>'],
        'client_shutdown': [],
        'client_force_stop': [],
        'client_response': ['ClientResponse'],
        'client_ready': ['rrr::i32'],
        'client_ready_block': ['rrr::i32'],
        'client_start': [],
        'DispatchTxn': ['TxReply'],
    }

    def __bind_helper__(self, func):
        def f(*args):
            return getattr(self, func.__name__)(*args)
        return f

    def __reg_to__(self, server):
        server.__reg_func__(ClientControlService.CLIENT_GET_TXN_NAMES, self.__bind_helper__(self.client_get_txn_names), [], ['std::map<rrr::i32, std::string>'])
        server.__reg_func__(ClientControlService.CLIENT_SHUTDOWN, self.__bind_helper__(self.client_shutdown), [], [])
        server.__reg_func__(ClientControlService.CLIENT_FORCE_STOP, self.__bind_helper__(self.client_force_stop), [], [])
        server.__reg_func__(ClientControlService.CLIENT_RESPONSE, self.__bind_helper__(self.client_response), ['DepId'], ['ClientResponse'])
        server.__reg_func__(ClientControlService.CLIENT_READY, self.__bind_helper__(self.client_ready), [], ['rrr::i32'])
        server.__reg_func__(ClientControlService.CLIENT_READY_BLOCK, self.__bind_helper__(self.client_ready_block), [], ['rrr::i32'])
        server.__reg_func__(ClientControlService.CLIENT_START, self.__bind_helper__(self.client_start), [], [])
        server.__reg_func__(ClientControlService.DISPATCHTXN, self.__bind_helper__(self.DispatchTxn), ['TxDispatchRequest'], ['TxReply'])

    def client_get_txn_names(__self__):
        raise NotImplementedError('subclass ClientControlService and implement your own client_get_txn_names function')

    def client_shutdown(__self__):
        raise NotImplementedError('subclass ClientControlService and implement your own client_shutdown function')

    def client_force_stop(__self__):
        raise NotImplementedError('subclass ClientControlService and implement your own client_force_stop function')

    def client_response(__self__, dep_id):
        raise NotImplementedError('subclass ClientControlService and implement your own client_response function')

    def client_ready(__self__):
        raise NotImplementedError('subclass ClientControlService and implement your own client_ready function')

    def client_ready_block(__self__):
        raise NotImplementedError('subclass ClientControlService and implement your own client_ready_block function')

    def client_start(__self__):
        raise NotImplementedError('subclass ClientControlService and implement your own client_start function')

    def DispatchTxn(__self__, req):
        raise NotImplementedError('subclass ClientControlService and implement your own DispatchTxn function')

class ClientControlProxy(object):
    def __init__(self, clnt):
        self.__clnt__ = clnt

    def async_client_get_txn_names(__self__):
        return __self__.__clnt__.async_call(ClientControlService.CLIENT_GET_TXN_NAMES, [], ClientControlService.__input_type_info__['client_get_txn_names'], ClientControlService.__output_type_info__['client_get_txn_names'])

    def async_client_shutdown(__self__):
        return __self__.__clnt__.async_call(ClientControlService.CLIENT_SHUTDOWN, [], ClientControlService.__input_type_info__['client_shutdown'], ClientControlService.__output_type_info__['client_shutdown'])

    def async_client_force_stop(__self__):
        return __self__.__clnt__.async_call(ClientControlService.CLIENT_FORCE_STOP, [], ClientControlService.__input_type_info__['client_force_stop'], ClientControlService.__output_type_info__['client_force_stop'])

    def async_client_response(__self__, dep_id):
        return __self__.__clnt__.async_call(ClientControlService.CLIENT_RESPONSE, [dep_id], ClientControlService.__input_type_info__['client_response'], ClientControlService.__output_type_info__['client_response'])

    def async_client_ready(__self__):
        return __self__.__clnt__.async_call(ClientControlService.CLIENT_READY, [], ClientControlService.__input_type_info__['client_ready'], ClientControlService.__output_type_info__['client_ready'])

    def async_client_ready_block(__self__):
        return __self__.__clnt__.async_call(ClientControlService.CLIENT_READY_BLOCK, [], ClientControlService.__input_type_info__['client_ready_block'], ClientControlService.__output_type_info__['client_ready_block'])

    def async_client_start(__self__):
        return __self__.__clnt__.async_call(ClientControlService.CLIENT_START, [], ClientControlService.__input_type_info__['client_start'], ClientControlService.__output_type_info__['client_start'])

    def async_DispatchTxn(__self__, req):
        return __self__.__clnt__.async_call(ClientControlService.DISPATCHTXN, [req], ClientControlService.__input_type_info__['DispatchTxn'], ClientControlService.__output_type_info__['DispatchTxn'])

    def sync_client_get_txn_names(__self__):
        __result__ = __self__.__clnt__.sync_call(ClientControlService.CLIENT_GET_TXN_NAMES, [], ClientControlService.__input_type_info__['client_get_txn_names'], ClientControlService.__output_type_info__['client_get_txn_names'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_client_shutdown(__self__):
        __result__ = __self__.__clnt__.sync_call(ClientControlService.CLIENT_SHUTDOWN, [], ClientControlService.__input_type_info__['client_shutdown'], ClientControlService.__output_type_info__['client_shutdown'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_client_force_stop(__self__):
        __result__ = __self__.__clnt__.sync_call(ClientControlService.CLIENT_FORCE_STOP, [], ClientControlService.__input_type_info__['client_force_stop'], ClientControlService.__output_type_info__['client_force_stop'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_client_response(__self__, dep_id):
        __result__ = __self__.__clnt__.sync_call(ClientControlService.CLIENT_RESPONSE, [dep_id], ClientControlService.__input_type_info__['client_response'], ClientControlService.__output_type_info__['client_response'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_client_ready(__self__):
        __result__ = __self__.__clnt__.sync_call(ClientControlService.CLIENT_READY, [], ClientControlService.__input_type_info__['client_ready'], ClientControlService.__output_type_info__['client_ready'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_client_ready_block(__self__):
        __result__ = __self__.__clnt__.sync_call(ClientControlService.CLIENT_READY_BLOCK, [], ClientControlService.__input_type_info__['client_ready_block'], ClientControlService.__output_type_info__['client_ready_block'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_client_start(__self__):
        __result__ = __self__.__clnt__.sync_call(ClientControlService.CLIENT_START, [], ClientControlService.__input_type_info__['client_start'], ClientControlService.__output_type_info__['client_start'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_DispatchTxn(__self__, req):
        __result__ = __self__.__clnt__.sync_call(ClientControlService.DISPATCHTXN, [req], ClientControlService.__input_type_info__['DispatchTxn'], ClientControlService.__output_type_info__['DispatchTxn'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

class ConfigServiceService(object):
    GETCONFIG = 0x36c2eab5
    GETCONFIGVERSION = 0x5b0d2a03
    HASCONFIG = 0x56e4d6a2
    SETSHARDINGPOLICY = 0x37fd2ac8
    GETSHARDINGPOLICY = 0x4d0d603e
    GETSHARDINGPOLICYVERSION = 0x269d5fe0
    HASSHARDINGPOLICY = 0x4415359d

    __input_type_info__ = {
        'GetConfig': ['uint64_t'],
        'GetConfigVersion': [],
        'HasConfig': [],
        'SetShardingPolicy': ['std::string'],
        'GetShardingPolicy': ['uint64_t'],
        'GetShardingPolicyVersion': [],
        'HasShardingPolicy': [],
    }

    __output_type_info__ = {
        'GetConfig': ['uint64_t','rrr::i32','std::string'],
        'GetConfigVersion': ['uint64_t'],
        'HasConfig': ['rrr::i32'],
        'SetShardingPolicy': ['rrr::i32'],
        'GetShardingPolicy': ['uint64_t','rrr::i32','std::string'],
        'GetShardingPolicyVersion': ['uint64_t'],
        'HasShardingPolicy': ['rrr::i32'],
    }

    def __bind_helper__(self, func):
        def f(*args):
            return getattr(self, func.__name__)(*args)
        return f

    def __reg_to__(self, server):
        server.__reg_func__(ConfigServiceService.GETCONFIG, self.__bind_helper__(self.GetConfig), ['uint64_t'], ['uint64_t','rrr::i32','std::string'])
        server.__reg_func__(ConfigServiceService.GETCONFIGVERSION, self.__bind_helper__(self.GetConfigVersion), [], ['uint64_t'])
        server.__reg_func__(ConfigServiceService.HASCONFIG, self.__bind_helper__(self.HasConfig), [], ['rrr::i32'])
        server.__reg_func__(ConfigServiceService.SETSHARDINGPOLICY, self.__bind_helper__(self.SetShardingPolicy), ['std::string'], ['rrr::i32'])
        server.__reg_func__(ConfigServiceService.GETSHARDINGPOLICY, self.__bind_helper__(self.GetShardingPolicy), ['uint64_t'], ['uint64_t','rrr::i32','std::string'])
        server.__reg_func__(ConfigServiceService.GETSHARDINGPOLICYVERSION, self.__bind_helper__(self.GetShardingPolicyVersion), [], ['uint64_t'])
        server.__reg_func__(ConfigServiceService.HASSHARDINGPOLICY, self.__bind_helper__(self.HasShardingPolicy), [], ['rrr::i32'])

    def GetConfig(__self__, client_version):
        raise NotImplementedError('subclass ConfigServiceService and implement your own GetConfig function')

    def GetConfigVersion(__self__):
        raise NotImplementedError('subclass ConfigServiceService and implement your own GetConfigVersion function')

    def HasConfig(__self__):
        raise NotImplementedError('subclass ConfigServiceService and implement your own HasConfig function')

    def SetShardingPolicy(__self__, policy_data):
        raise NotImplementedError('subclass ConfigServiceService and implement your own SetShardingPolicy function')

    def GetShardingPolicy(__self__, client_version):
        raise NotImplementedError('subclass ConfigServiceService and implement your own GetShardingPolicy function')

    def GetShardingPolicyVersion(__self__):
        raise NotImplementedError('subclass ConfigServiceService and implement your own GetShardingPolicyVersion function')

    def HasShardingPolicy(__self__):
        raise NotImplementedError('subclass ConfigServiceService and implement your own HasShardingPolicy function')

class ConfigServiceProxy(object):
    def __init__(self, clnt):
        self.__clnt__ = clnt

    def async_GetConfig(__self__, client_version):
        return __self__.__clnt__.async_call(ConfigServiceService.GETCONFIG, [client_version], ConfigServiceService.__input_type_info__['GetConfig'], ConfigServiceService.__output_type_info__['GetConfig'])

    def async_GetConfigVersion(__self__):
        return __self__.__clnt__.async_call(ConfigServiceService.GETCONFIGVERSION, [], ConfigServiceService.__input_type_info__['GetConfigVersion'], ConfigServiceService.__output_type_info__['GetConfigVersion'])

    def async_HasConfig(__self__):
        return __self__.__clnt__.async_call(ConfigServiceService.HASCONFIG, [], ConfigServiceService.__input_type_info__['HasConfig'], ConfigServiceService.__output_type_info__['HasConfig'])

    def async_SetShardingPolicy(__self__, policy_data):
        return __self__.__clnt__.async_call(ConfigServiceService.SETSHARDINGPOLICY, [policy_data], ConfigServiceService.__input_type_info__['SetShardingPolicy'], ConfigServiceService.__output_type_info__['SetShardingPolicy'])

    def async_GetShardingPolicy(__self__, client_version):
        return __self__.__clnt__.async_call(ConfigServiceService.GETSHARDINGPOLICY, [client_version], ConfigServiceService.__input_type_info__['GetShardingPolicy'], ConfigServiceService.__output_type_info__['GetShardingPolicy'])

    def async_GetShardingPolicyVersion(__self__):
        return __self__.__clnt__.async_call(ConfigServiceService.GETSHARDINGPOLICYVERSION, [], ConfigServiceService.__input_type_info__['GetShardingPolicyVersion'], ConfigServiceService.__output_type_info__['GetShardingPolicyVersion'])

    def async_HasShardingPolicy(__self__):
        return __self__.__clnt__.async_call(ConfigServiceService.HASSHARDINGPOLICY, [], ConfigServiceService.__input_type_info__['HasShardingPolicy'], ConfigServiceService.__output_type_info__['HasShardingPolicy'])

    def sync_GetConfig(__self__, client_version):
        __result__ = __self__.__clnt__.sync_call(ConfigServiceService.GETCONFIG, [client_version], ConfigServiceService.__input_type_info__['GetConfig'], ConfigServiceService.__output_type_info__['GetConfig'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_GetConfigVersion(__self__):
        __result__ = __self__.__clnt__.sync_call(ConfigServiceService.GETCONFIGVERSION, [], ConfigServiceService.__input_type_info__['GetConfigVersion'], ConfigServiceService.__output_type_info__['GetConfigVersion'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_HasConfig(__self__):
        __result__ = __self__.__clnt__.sync_call(ConfigServiceService.HASCONFIG, [], ConfigServiceService.__input_type_info__['HasConfig'], ConfigServiceService.__output_type_info__['HasConfig'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_SetShardingPolicy(__self__, policy_data):
        __result__ = __self__.__clnt__.sync_call(ConfigServiceService.SETSHARDINGPOLICY, [policy_data], ConfigServiceService.__input_type_info__['SetShardingPolicy'], ConfigServiceService.__output_type_info__['SetShardingPolicy'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_GetShardingPolicy(__self__, client_version):
        __result__ = __self__.__clnt__.sync_call(ConfigServiceService.GETSHARDINGPOLICY, [client_version], ConfigServiceService.__input_type_info__['GetShardingPolicy'], ConfigServiceService.__output_type_info__['GetShardingPolicy'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_GetShardingPolicyVersion(__self__):
        __result__ = __self__.__clnt__.sync_call(ConfigServiceService.GETSHARDINGPOLICYVERSION, [], ConfigServiceService.__input_type_info__['GetShardingPolicyVersion'], ConfigServiceService.__output_type_info__['GetShardingPolicyVersion'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

    def sync_HasShardingPolicy(__self__):
        __result__ = __self__.__clnt__.sync_call(ConfigServiceService.HASSHARDINGPOLICY, [], ConfigServiceService.__input_type_info__['HasShardingPolicy'], ConfigServiceService.__output_type_info__['HasShardingPolicy'])
        if __result__[0] != 0:
            raise Exception("RPC returned non-zero error code %d: %s" % (__result__[0], os.strerror(__result__[0])))
        if len(__result__[1]) == 1:
            return __result__[1][0]
        elif len(__result__[1]) > 1:
            return __result__[1]

