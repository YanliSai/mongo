/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <utility>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/json.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/find_and_modify_request.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/s/catalog/catalog_manager_mock.h"
#include "mongo/s/catalog/dist_lock_catalog_impl.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/type_lockpings.h"
#include "mongo/s/type_locks.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace mongo {

using std::vector;
using executor::NetworkInterfaceMock;
using executor::NetworkTestEnv;

namespace {

const HostAndPort dummyHost("dummy", 123);
const Milliseconds kWTimeout(100);

/**
 * Sets up the mocked out objects for testing the replica-set backed catalog manager.
 */
class DistLockCatalogFixture : public mongo::unittest::Test {
public:
    template <typename Lambda>
    executor::NetworkTestEnv::FutureHandle<typename std::result_of<Lambda()>::type> launchAsync(
        Lambda&& func) const {
        return _networkTestEnv->launchAsync(std::forward<Lambda>(func));
    }

    void onCommand(NetworkTestEnv::OnCommandFunction func) {
        _networkTestEnv->onCommand(func);
    }

    void onFindCommand(NetworkTestEnv::OnFindCommandFunction func) {
        _networkTestEnv->onFindCommand(func);
    }

    RemoteCommandTargeterMock* targeter() {
        return &_targeter;
    }

    DistLockCatalogImpl* catalog() {
        return _distLockCatalog.get();
    }

    ShardRegistry* shardRegistry() {
        return _shardRegistry.get();
    }

private:
    void setUp() override {
        _targeter.setFindHostReturnValue(dummyHost);

        auto networkUniquePtr = stdx::make_unique<executor::NetworkInterfaceMock>();
        executor::NetworkInterfaceMock* network = networkUniquePtr.get();
        auto executor =
            stdx::make_unique<repl::ReplicationExecutor>(networkUniquePtr.release(), nullptr, 0);

        _networkTestEnv = stdx::make_unique<NetworkTestEnv>(executor.get(), network);

        _shardRegistry =
            stdx::make_unique<ShardRegistry>(stdx::make_unique<RemoteCommandTargeterFactoryMock>(),
                                             std::move(executor),
                                             network,
                                             &_catalogMgr);
        _shardRegistry->startup();

        _distLockCatalog =
            stdx::make_unique<DistLockCatalogImpl>(&_targeter, _shardRegistry.get(), kWTimeout);
    }

    void tearDown() override {
        shardRegistry()->shutdown();
    }

    std::unique_ptr<executor::NetworkTestEnv> _networkTestEnv;

    RemoteCommandTargeterMock _targeter;
    CatalogManagerMock _catalogMgr;

    std::unique_ptr<ShardRegistry> _shardRegistry;
    std::unique_ptr<DistLockCatalogImpl> _distLockCatalog;
};

TEST_F(DistLockCatalogFixture, BasicPing) {
    auto future = launchAsync([this] {
        Date_t ping(dateFromISOString("2014-03-11T09:17:18.098Z").getValue());
        auto status = catalog()->ping("abcd", ping);
        ASSERT_OK(status);
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
                findAndModify: "lockpings",
                query: { _id: "abcd" },
                update: {
                    $set: {
                        ping: { $date: "2014-03-11T09:17:18.098Z" }
                    }
                },
                upsert: true,
                writeConcern: { w: "majority", j: true, wtimeout: 100 }
            })"));

        ASSERT_EQUALS(expectedCmd, request.cmdObj);

        return fromjson(R"({
                ok: 1,
                value: {
                    _id: "abcd",
                    ping: { $date: "2014-03-11T09:17:18.098Z" }
                }
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, PingTargetError) {
    targeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = catalog()->ping("abcd", Date_t::now());
    ASSERT_NOT_OK(status);
}

TEST_F(DistLockCatalogFixture, PingRunnerError) {
    auto future = launchAsync([this] {
        auto status = catalog()->ping("abcd", Date_t::now());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return {ErrorCodes::InternalError, "Bad"};
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, PingCommandError) {
    auto future = launchAsync([this] {
        auto status = catalog()->ping("abcd", Date_t::now());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                errmsg: "bad",
                code: 9
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, PingWriteError) {
    auto future = launchAsync([this] {
        auto status = catalog()->ping("abcd", Date_t::now());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::DuplicateKey, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                code: 11000,
                errmsg: "E11000 duplicate key error"
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, PingWriteConcernError) {
    auto future = launchAsync([this] {
        auto status = catalog()->ping("abcd", Date_t::now());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: {
                    code: 64,
                    errmsg: "waiting for replication timed out"
                }
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, PingUnsupportedWriteConcernResponse) {
    auto future = launchAsync([this] {
        auto status = catalog()->ping("abcd", Date_t::now());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: {
                    code: "bad format",
                    errmsg: "waiting for replication timed out"
                }
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, PingUnsupportedResponseFormat) {
    auto future = launchAsync([this] {
        auto status = catalog()->ping("abcd", Date_t::now());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return BSON("ok" << 1 << "value"
                         << "NaN");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, GrabLockNoOp) {
    auto future = launchAsync([this] {
        OID myID("555f80be366c194b13fb0372");
        Date_t now(dateFromISOString("2015-05-22T19:17:18.098Z").getValue());
        auto resultStatus =
            catalog()->grabLock("test", myID, "me", "mongos", now, "because").getStatus();

        ASSERT_NOT_OK(resultStatus);
        ASSERT_EQUALS(ErrorCodes::LockStateChangeFailed, resultStatus.code());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
                findAndModify: "locks",
                query: { _id: "test", state: 0 },
                update: {
                    $set: {
                        ts: ObjectId("555f80be366c194b13fb0372"),
                        state: 2,
                        who: "me",
                        process: "mongos",
                        when: { $date: "2015-05-22T19:17:18.098Z" },
                        why: "because"
                    }
                },
                upsert: true,
                new: true,
                writeConcern: { w: "majority", j: true, wtimeout: 100 }
            })"));

        ASSERT_EQUALS(expectedCmd, request.cmdObj);

        return fromjson("{ ok: 1, value: null }");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, GrabLockWithNewDoc) {
    auto future = launchAsync([this] {
        OID myID("555f80be366c194b13fb0372");
        Date_t now(dateFromISOString("2015-05-22T19:17:18.098Z").getValue());
        auto resultStatus = catalog()->grabLock("test", myID, "me", "mongos", now, "because");
        ASSERT_OK(resultStatus.getStatus());

        const auto& lockDoc = resultStatus.getValue();
        ASSERT_TRUE(lockDoc.isValid(nullptr));
        ASSERT_EQUALS("test", lockDoc.getName());
        ASSERT_EQUALS(myID, lockDoc.getLockID());
        ASSERT_EQUALS("me", lockDoc.getWho());
        ASSERT_EQUALS("mongos", lockDoc.getProcess());
        ASSERT_EQUALS("because", lockDoc.getWhy());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
                findAndModify: "locks",
                query: { _id: "test", state: 0 },
                update: {
                    $set: {
                        ts: ObjectId("555f80be366c194b13fb0372"),
                        state: 2,
                        who: "me",
                        process: "mongos",
                        when: { $date: "2015-05-22T19:17:18.098Z" },
                        why: "because"
                    }
                },
                upsert: true,
                new: true,
                writeConcern: { w: "majority", j: true, wtimeout: 100 }
            })"));

        ASSERT_EQUALS(expectedCmd, request.cmdObj);

        return fromjson(R"({
                lastErrorObject: {
                    updatedExisting: false,
                    n: 1,
                    upserted: 1
                },
                value: {
                    _id: "test",
                    ts: ObjectId("555f80be366c194b13fb0372"),
                    state: 2,
                    who: "me",
                    process: "mongos",
                    when: { $date: "2015-05-22T19:17:18.098Z" },
                    why: "because"
                },
                ok: 1
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, GrabLockTargetError) {
    targeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = catalog()->grabLock("", OID::gen(), "", "", Date_t::now(), "").getStatus();
    ASSERT_NOT_OK(status);
}

TEST_F(DistLockCatalogFixture, GrabLockRunnerError) {
    auto future = launchAsync([this] {
        auto status = catalog()->grabLock("", OID::gen(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return {ErrorCodes::InternalError, "Bad"};
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, GrabLockCommandError) {
    auto future = launchAsync([this] {
        auto status = catalog()->grabLock("", OID::gen(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                errmsg: "bad",
                code: 9
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, GrabLockWriteError) {
    auto future = launchAsync([this] {
        auto status = catalog()->grabLock("", OID::gen(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::DuplicateKey, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                code: 11000,
                errmsg: "E11000 duplicate key error"
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, GrabLockWriteConcernError) {
    auto future = launchAsync([this] {
        auto status = catalog()->grabLock("", OID::gen(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: {
                    code: 64,
                    errmsg: "waiting for replication timed out"
                }
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, GrabLockUnsupportedWriteConcernResponse) {
    auto future = launchAsync([this] {
        auto status = catalog()->grabLock("", OID::gen(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: {
                    code: "bad format",
                    errmsg: "waiting for replication timed out"
                }
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, GrabLockUnsupportedResponseFormat) {
    auto future = launchAsync([this] {
        auto status = catalog()->grabLock("", OID::gen(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return BSON("ok" << 1 << "value"
                         << "NaN");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, OvertakeLockNoOp) {
    auto future = launchAsync([this] {
        OID myID("555f80be366c194b13fb0372");
        OID currentOwner("555f99712c99a78c5b083358");
        Date_t now(dateFromISOString("2015-05-22T19:17:18.098Z").getValue());
        auto resultStatus =
            catalog()
                ->overtakeLock("test", myID, currentOwner, "me", "mongos", now, "because")
                .getStatus();

        ASSERT_NOT_OK(resultStatus);
        ASSERT_EQUALS(ErrorCodes::LockStateChangeFailed, resultStatus.code());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
                findAndModify: "locks",
                query: {
                    $or: [
                        { _id: "test", state: 0 },
                        { _id: "test", ts: ObjectId("555f99712c99a78c5b083358") }
                    ]
                },
                update: {
                    $set: {
                        ts: ObjectId("555f80be366c194b13fb0372"),
                        state: 2,
                        who: "me",
                        process: "mongos",
                        when: { $date: "2015-05-22T19:17:18.098Z" },
                        why: "because"
                    }
                },
                new: true,
                writeConcern: { w: "majority", j: true, wtimeout: 100 }
            })"));

        ASSERT_EQUALS(expectedCmd, request.cmdObj);

        return fromjson("{ ok: 1, value: null }");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, OvertakeLockWithNewDoc) {
    auto future = launchAsync([this] {
        OID myID("555f80be366c194b13fb0372");
        OID currentOwner("555f99712c99a78c5b083358");
        Date_t now(dateFromISOString("2015-05-22T19:17:18.098Z").getValue());
        auto resultStatus =
            catalog()->overtakeLock("test", myID, currentOwner, "me", "mongos", now, "because");
        ASSERT_OK(resultStatus.getStatus());

        const auto& lockDoc = resultStatus.getValue();
        ASSERT_TRUE(lockDoc.isValid(nullptr));
        ASSERT_EQUALS("test", lockDoc.getName());
        ASSERT_EQUALS(myID, lockDoc.getLockID());
        ASSERT_EQUALS("me", lockDoc.getWho());
        ASSERT_EQUALS("mongos", lockDoc.getProcess());
        ASSERT_EQUALS("because", lockDoc.getWhy());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
                findAndModify: "locks",
                query: {
                    $or: [
                        { _id: "test", state: 0 },
                        { _id: "test", ts: ObjectId("555f99712c99a78c5b083358") }
                    ]
                },
                update: {
                    $set: {
                        ts: ObjectId("555f80be366c194b13fb0372"),
                        state: 2,
                        who: "me",
                        process: "mongos",
                        when: { $date: "2015-05-22T19:17:18.098Z" },
                        why: "because"
                    }
                },
                new: true,
                writeConcern: { w: "majority", j: true, wtimeout: 100 }
            })"));

        ASSERT_EQUALS(expectedCmd, request.cmdObj);

        return fromjson(R"({
                lastErrorObject: {
                    updatedExisting: false,
                    n: 1,
                    upserted: 1
                },
                value: {
                    _id: "test",
                    ts: ObjectId("555f80be366c194b13fb0372"),
                    state: 2,
                    who: "me",
                    process: "mongos",
                    when: { $date: "2015-05-22T19:17:18.098Z" },
                    why: "because"
                },
                ok: 1
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, OvertakeLockTargetError) {
    targeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = catalog()->overtakeLock("", OID(), OID(), "", "", Date_t::now(), "").getStatus();
    ASSERT_NOT_OK(status);
}

TEST_F(DistLockCatalogFixture, OvertakeLockRunnerError) {
    auto future = launchAsync([this] {
        auto status =
            catalog()->overtakeLock("", OID(), OID(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return {ErrorCodes::InternalError, "Bad"};
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, OvertakeLockCommandError) {
    auto future = launchAsync([this] {
        auto status =
            catalog()->overtakeLock("", OID(), OID(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                errmsg: "bad",
                code: 9
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, OvertakeLockWriteError) {
    auto future = launchAsync([this] {
        auto status =
            catalog()->overtakeLock("", OID(), OID(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::DuplicateKey, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                code: 11000,
                errmsg: "E11000 duplicate key error"
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, OvertakeLockWriteConcernError) {
    auto future = launchAsync([this] {
        auto status =
            catalog()->overtakeLock("", OID(), OID(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: {
                    code: 64,
                    errmsg: "waiting for replication timed out"
                }
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, OvertakeLockUnsupportedWriteConcernResponse) {
    auto future = launchAsync([this] {
        auto status =
            catalog()->overtakeLock("", OID(), OID(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: {
                    code: "bad format",
                    errmsg: "waiting for replication timed out"
                }
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, OvertakeLockUnsupportedResponseFormat) {
    auto future = launchAsync([this] {
        auto status =
            catalog()->overtakeLock("", OID(), OID(), "", "", Date_t::now(), "").getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return BSON("ok" << 1 << "value"
                         << "NaN");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, BasicUnlock) {
    auto future = launchAsync([this] {
        auto status = catalog()->unlock(OID("555f99712c99a78c5b083358"));
        ASSERT_OK(status);
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
                findAndModify: "locks",
                query: { ts: ObjectId("555f99712c99a78c5b083358") },
                update: { $set: { state: 0 }},
                writeConcern: { w: "majority", j: true, wtimeout: 100 }
            })"));

        ASSERT_EQUALS(expectedCmd, request.cmdObj);

        return fromjson(R"({
                ok: 1,
                value: {
                    _id: "",
                    ts: ObjectId("555f99712c99a78c5b083358"),
                    state: 0
                }
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, UnlockWithNoNewDoc) {
    auto future = launchAsync([this] {
        auto status = catalog()->unlock(OID("555f99712c99a78c5b083358"));
        ASSERT_OK(status);
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
                findAndModify: "locks",
                query: { ts: ObjectId("555f99712c99a78c5b083358") },
                update: { $set: { state: 0 }},
                writeConcern: { w: "majority", j: true, wtimeout: 100 }
            })"));

        ASSERT_EQUALS(expectedCmd, request.cmdObj);

        return fromjson(R"({
                ok: 1,
                value: null
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, UnlockTargetError) {
    targeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = catalog()->unlock(OID());
    ASSERT_NOT_OK(status);
}

TEST_F(DistLockCatalogFixture, UnlockRunnerError) {
    auto future = launchAsync([this] {
        auto status = catalog()->unlock(OID());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return {ErrorCodes::InternalError, "Bad"};
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, UnlockCommandError) {
    auto future = launchAsync([this] {
        auto status = catalog()->unlock(OID());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                errmsg: "bad",
                code: 9
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, UnlockWriteError) {
    auto future = launchAsync([this] {
        auto status = catalog()->unlock(OID());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::DuplicateKey, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                code: 11000,
                errmsg: "E11000 duplicate key error"
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, UnlockWriteConcernError) {
    auto future = launchAsync([this] {
        auto status = catalog()->unlock(OID());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: {
                    code: 64,
                    errmsg: "waiting for replication timed out"
                }
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, UnlockUnsupportedWriteConcernResponse) {
    auto future = launchAsync([this] {
        auto status = catalog()->unlock(OID());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: {
                    code: "bad format",
                    errmsg: "waiting for replication timed out"
                }
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, UnlockUnsupportedResponseFormat) {
    auto future = launchAsync([this] {
        auto status = catalog()->unlock(OID());
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return BSON("ok" << 1 << "value"
                         << "NaN");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, BasicGetServerInfo) {
    auto future = launchAsync([this] {
        Date_t localTime(dateFromISOString("2015-05-26T13:06:27.293Z").getValue());
        OID electionID("555fa85d4d8640862a0fc79b");
        auto resultStatus = catalog()->getServerInfo();
        ASSERT_OK(resultStatus.getStatus());

        const auto& serverInfo = resultStatus.getValue();
        ASSERT_EQUALS(electionID, serverInfo.electionId);
        ASSERT_EQUALS(localTime, serverInfo.serverTime);
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("admin", request.dbname);
        ASSERT_EQUALS(BSON("serverStatus" << 1), request.cmdObj);

        return fromjson(R"({
                localTime: { $date: "2015-05-26T13:06:27.293Z" },
                $gleStats: {
                    lastOpTime: { $timestamp: { t: 0, i: 0 }},
                    electionId: ObjectId("555fa85d4d8640862a0fc79b")
                },
                ok: 1
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, GetServerTargetError) {
    targeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = catalog()->getServerInfo().getStatus();
    ASSERT_NOT_OK(status);
}

TEST_F(DistLockCatalogFixture, GetServerRunnerError) {
    auto future = launchAsync([this] {
        auto status = catalog()->getServerInfo().getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return {ErrorCodes::InternalError, "Bad"};
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, GetServerCommandError) {
    auto future = launchAsync([this] {
        auto status = catalog()->getServerInfo().getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                errmsg: "bad",
                code: 9
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, GetServerBadElectionId) {
    auto future = launchAsync([this] {
        auto status = catalog()->getServerInfo().getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                localTime: { $date: "2015-05-26T13:06:27.293Z" },
                $gleStats: {
                    lastOpTime: { $timestamp: { t: 0, i: 0 }},
                    electionId: 34
                },
                ok: 1
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, GetServerBadLocalTime) {
    auto future = launchAsync([this] {
        auto status = catalog()->getServerInfo().getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                localTime: "2015-05-26T13:06:27.293Z",
                $gleStats: {
                    lastOpTime: { $timestamp: { t: 0, i: 0 }},
                    electionId: ObjectId("555fa85d4d8640862a0fc79b")
                },
                ok: 1
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, GetServerNoGLEStats) {
    auto future = launchAsync([this] {
        auto status = catalog()->getServerInfo().getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                localTime: { $date: "2015-05-26T13:06:27.293Z" },
                ok: 1
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, GetServerNoElectionId) {
    auto future = launchAsync([this] {
        auto status = catalog()->getServerInfo().getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                localTime: { $date: "2015-05-26T13:06:27.293Z" },
                $gleStats: {
                    lastOpTime: { $timestamp: { t: 0, i: 0 }},
                    termNumber: 64
                },
                ok: 1
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, BasicStopPing) {
    auto future = launchAsync([this] {
        auto status = catalog()->stopPing("test");
        ASSERT_OK(status);
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
                findAndModify: "lockpings",
                query: { _id: "test" },
                remove: true,
                writeConcern: { w: "majority", j: true, wtimeout: 100 }
            })"));

        ASSERT_EQUALS(expectedCmd, request.cmdObj);

        return fromjson(R"({
                ok: 1,
                value: {
                  _id: "test",
                  ping: { $date: "2014-03-11T09:17:18.098Z" }
                }
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, StopPingTargetError) {
    targeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = catalog()->stopPing("");
    ASSERT_NOT_OK(status);
}

TEST_F(DistLockCatalogFixture, StopPingRunnerError) {
    auto future = launchAsync([this] {
        auto status = catalog()->stopPing("");
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return {ErrorCodes::InternalError, "Bad"};
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, StopPingCommandError) {
    auto future = launchAsync([this] {
        auto status = catalog()->stopPing("");
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                errmsg: "bad",
                code: 9
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, StopPingWriteError) {
    auto future = launchAsync([this] {
        auto status = catalog()->stopPing("");
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::Unauthorized, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 0,
                code: 13,
                errmsg: "Unauthorized"
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, StopPingWriteConcernError) {
    auto future = launchAsync([this] {
        auto status = catalog()->stopPing("");
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: {
                    code: 64,
                    errmsg: "waiting for replication timed out"
                }
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, StopPingUnsupportedWriteConcernResponse) {
    auto future = launchAsync([this] {
        auto status = catalog()->stopPing("");
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return fromjson(R"({
                ok: 1,
                value: null,
                writeConcernError: {
                    code: "bad format",
                    errmsg: "waiting for replication timed out"
                }
            })");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, StopPingUnsupportedResponseFormat) {
    auto future = launchAsync([this] {
        auto status = catalog()->stopPing("");
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
    });

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return BSON("ok" << 1 << "value"
                         << "NaN");
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, BasicGetPing) {
    auto future = async(std::launch::async,
                        [this] {
                            Date_t ping(dateFromISOString("2015-05-26T13:06:27.293Z").getValue());
                            auto resultStatus = catalog()->getPing("test");
                            ASSERT_OK(resultStatus.getStatus());

                            const auto& pingDoc = resultStatus.getValue();
                            ASSERT_EQUALS("test", pingDoc.getProcess());
                            ASSERT_EQUALS(ping, pingDoc.getPing());
                        });

    onFindCommand([](const RemoteCommandRequest& request) -> StatusWith<vector<BSONObj>> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
            find: "lockpings",
            filter: { _id: "test" },
            limit: 1
        })"));

        ASSERT_EQUALS(expectedCmd, request.cmdObj);

        BSONObj pingDoc(fromjson(R"({
            _id: "test",
            ping: { $date: "2015-05-26T13:06:27.293Z" }
        })"));

        std::vector<BSONObj> result;
        result.push_back(pingDoc);

        return result;
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, GetPingTargetError) {
    targeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = catalog()->getPing("").getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
}

TEST_F(DistLockCatalogFixture, GetPingRunnerError) {
    auto future = async(std::launch::async,
                        [this] {
                            auto status = catalog()->getPing("").getStatus();
                            ASSERT_NOT_OK(status);
                            ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
                            ASSERT_FALSE(status.reason().empty());
                        });

    onFindCommand([](const RemoteCommandRequest& request) -> StatusWith<vector<BSONObj>> {
        return {ErrorCodes::InternalError, "Bad"};
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, GetPingNotFound) {
    auto future = async(std::launch::async,
                        [this] {
                            auto status = catalog()->getPing("").getStatus();
                            ASSERT_NOT_OK(status);
                            ASSERT_EQUALS(ErrorCodes::NoMatchingDocument, status.code());
                            ASSERT_FALSE(status.reason().empty());
                        });

    onFindCommand([](const RemoteCommandRequest& request)
                      -> StatusWith<vector<BSONObj>> { return std::vector<BSONObj>(); });

    future.get();
}

TEST_F(DistLockCatalogFixture, GetPingUnsupportedFormat) {
    auto future = async(std::launch::async,
                        [this] {
                            auto status = catalog()->getPing("test").getStatus();
                            ASSERT_NOT_OK(status);
                            ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
                            ASSERT_FALSE(status.reason().empty());
                        });

    onFindCommand([](const RemoteCommandRequest& request) -> StatusWith<vector<BSONObj>> {
        BSONObj pingDoc(fromjson(R"({
            _id: "test",
            ping: "bad"
        })"));

        std::vector<BSONObj> result;
        result.push_back(pingDoc);

        return result;
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, BasicGetLockByTS) {
    auto future = async(std::launch::async,
                        [this] {
                            OID ts("555f99712c99a78c5b083358");
                            auto resultStatus = catalog()->getLockByTS(ts);
                            ASSERT_OK(resultStatus.getStatus());

                            const auto& lockDoc = resultStatus.getValue();
                            ASSERT_EQUALS("test", lockDoc.getName());
                            ASSERT_EQUALS(ts, lockDoc.getLockID());
                        });

    onFindCommand([](const RemoteCommandRequest& request) -> StatusWith<vector<BSONObj>> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
            find: "locks",
            filter: { ts: ObjectId("555f99712c99a78c5b083358") },
            limit: 1
        })"));

        ASSERT_EQUALS(expectedCmd, request.cmdObj);

        BSONObj lockDoc(fromjson(R"({
            _id: "test",
            state: 2,
            ts: ObjectId("555f99712c99a78c5b083358")
        })"));

        std::vector<BSONObj> result;
        result.push_back(lockDoc);
        return result;
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, GetLockByTSTargetError) {
    targeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = catalog()->getLockByTS(OID()).getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
}

TEST_F(DistLockCatalogFixture, GetLockByTSRunnerError) {
    auto future = async(std::launch::async,
                        [this] {
                            auto status = catalog()->getLockByTS(OID()).getStatus();
                            ASSERT_NOT_OK(status);
                            ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
                            ASSERT_FALSE(status.reason().empty());
                        });

    onFindCommand([](const RemoteCommandRequest& request) -> StatusWith<vector<BSONObj>> {
        return {ErrorCodes::InternalError, "Bad"};
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, GetLockByTSNotFound) {
    auto future = async(std::launch::async,
                        [this] {
                            auto status = catalog()->getLockByTS(OID()).getStatus();
                            ASSERT_NOT_OK(status);
                            ASSERT_EQUALS(ErrorCodes::LockNotFound, status.code());
                            ASSERT_FALSE(status.reason().empty());
                        });

    onFindCommand([](const RemoteCommandRequest& request)
                      -> StatusWith<vector<BSONObj>> { return std::vector<BSONObj>(); });

    future.get();
}

TEST_F(DistLockCatalogFixture, GetLockByTSUnsupportedFormat) {
    auto future = async(std::launch::async,
                        [this] {
                            auto status = catalog()->getLockByTS(OID()).getStatus();
                            ASSERT_NOT_OK(status);
                            ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
                            ASSERT_FALSE(status.reason().empty());
                        });

    onFindCommand([](const RemoteCommandRequest& request) -> StatusWith<vector<BSONObj>> {
        BSONObj lockDoc(fromjson(R"({
            _id: "test",
            state: "bad"
        })"));

        std::vector<BSONObj> result;
        result.push_back(lockDoc);

        return result;
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, BasicGetLockByName) {
    auto future = async(std::launch::async,
                        [this] {
                            OID ts("555f99712c99a78c5b083358");
                            auto resultStatus = catalog()->getLockByName("abc");
                            ASSERT_OK(resultStatus.getStatus());

                            const auto& lockDoc = resultStatus.getValue();
                            ASSERT_EQUALS("abc", lockDoc.getName());
                            ASSERT_EQUALS(ts, lockDoc.getLockID());
                        });

    onFindCommand([](const RemoteCommandRequest& request) -> StatusWith<vector<BSONObj>> {
        ASSERT_EQUALS(dummyHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCmd(fromjson(R"({
            find: "locks",
            filter: { _id: "abc" },
            limit: 1
        })"));

        ASSERT_EQUALS(expectedCmd, request.cmdObj);

        BSONObj lockDoc(fromjson(R"({
            _id: "abc",
            state: 2,
            ts: ObjectId("555f99712c99a78c5b083358")
        })"));

        std::vector<BSONObj> result;
        result.push_back(lockDoc);
        return result;
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, GetLockByNameTargetError) {
    targeter()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"});
    auto status = catalog()->getLockByName("x").getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
}

TEST_F(DistLockCatalogFixture, GetLockByNameRunnerError) {
    auto future = async(std::launch::async,
                        [this] {
                            auto status = catalog()->getLockByName("x").getStatus();
                            ASSERT_NOT_OK(status);
                            ASSERT_EQUALS(ErrorCodes::InternalError, status.code());
                            ASSERT_FALSE(status.reason().empty());
                        });

    onFindCommand([](const RemoteCommandRequest& request) -> StatusWith<vector<BSONObj>> {
        return {ErrorCodes::InternalError, "Bad"};
    });

    future.get();
}

TEST_F(DistLockCatalogFixture, GetLockByNameNotFound) {
    auto future = async(std::launch::async,
                        [this] {
                            auto status = catalog()->getLockByName("x").getStatus();
                            ASSERT_NOT_OK(status);
                            ASSERT_EQUALS(ErrorCodes::LockNotFound, status.code());
                            ASSERT_FALSE(status.reason().empty());
                        });

    onFindCommand([](const RemoteCommandRequest& request)
                      -> StatusWith<vector<BSONObj>> { return std::vector<BSONObj>(); });

    future.get();
}

TEST_F(DistLockCatalogFixture, GetLockByNameUnsupportedFormat) {
    auto future = async(std::launch::async,
                        [this] {
                            auto status = catalog()->getLockByName("x").getStatus();
                            ASSERT_NOT_OK(status);
                            ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
                            ASSERT_FALSE(status.reason().empty());
                        });

    onFindCommand([](const RemoteCommandRequest& request) -> StatusWith<vector<BSONObj>> {
        BSONObj lockDoc(fromjson(R"({
            _id: "x",
            state: "bad"
        })"));

        std::vector<BSONObj> result;
        result.push_back(lockDoc);

        return result;
    });

    future.get();
}

}  // unnamed namespace
}  // namespace mongo
