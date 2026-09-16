// Who owns a subscription taken through the Qt consumer surface, and what ends
// it (logos-workspace#220).
//
// The surface had exactly one answer to both questions: the PROCESS owned it,
// and nothing ended it. `logos::qt::subscribe()` parked the handle in the
// process-lifetime LpBridge — "subscribe once, delivered forever" — with no
// counterpart to keep(), so a subscription could never be handed back.
//
// For a module that is right: a generated wrapper is a copyable handle, call
// sites subscribe on a `bind_x(...)` temporary, and a subscription that died
// with the temporary would deliver nothing. For a `ui_qml` VIEW it is wrong,
// and measurably so. A view backend is per-MOUNT: the host builds a LogosAPI
// for the mount and deletes it at unmount. Everything the backend subscribed
// stayed armed holding a freed `this`, and the next event from the provider
// went straight into it — SIGSEGV at KERN_INVALID_ADDRESS 0x61 in
// ChatBackend::applyDeliveryState, delivered from lp's event trampoline while
// the RE-OPENED mount was still waiting for its replica.
//
// So a subscription names its owner now, and these are the four things that
// must be true of that: an identity can drop what it took, dropping is
// idempotent, one identity's drop leaves another's alone, and — the floor under
// all of it — an identity that is DESTROYED takes its subscriptions with it
// whether or not anyone remembered to ask.
//
// No provider is running here, and none is needed: lp subscriptions arm when
// their target appears (onEventWhenAvailable), so one taken against an absent
// module is a live, deferred subscription — which is exactly the state a view's
// subscriptions are in for most of a mount.

#include <gtest/gtest.h>

#include <QString>

#include "logos_mock.h"
#include "logos_api.h"
#include "logos_qt_lp_bridge.h"

namespace {

class LpSubscriptionOwnershipTest : public ::testing::Test {
protected:
    void SetUp() override { m_mock = new LogosMockSetup(); }
    void TearDown() override { delete m_mock; }

    static logos::SubHandle subscribeOn(logos::qt::LpBridge* bridge,
                                        const char* event,
                                        const QObject* owner)
    {
        return logos::qt::subscribe(bridge, event, [](nlohmann::json) {}, owner);
    }

    LogosMockSetup* m_mock = nullptr;
};

// The premise every other case here rests on: a subscription taken against a
// module that is not running is LIVE, not refused. If this ever fails, the
// cases below would be asserting things about empty handles and would pass
// while proving nothing.
TEST_F(LpSubscriptionOwnershipTest, ASubscriptionArmsWithoutItsProvider)
{
    LogosAPI api(QStringLiteral("sub_own_probe"));
    logos::qt::LpBridge* bridge =
        logos::qt::LpBridge::forTarget(&api, QStringLiteral("sub_own_probe_target"));
    ASSERT_NE(bridge, nullptr);

    logos::SubHandle handle = subscribeOn(bridge, "anything", &api);
    EXPECT_TRUE(handle) << "an lp subscription is deferred until its provider "
                           "appears, never refused outright";
}

// THE assertion. A view backend's identity object can hand back everything it
// took, which is what the surface had no way to express at all.
TEST_F(LpSubscriptionOwnershipTest, AnIdentityCanDropWhatItSubscribed)
{
    LogosAPI api(QStringLiteral("sub_own_dropper"));
    logos::qt::LpBridge* bridge =
        logos::qt::LpBridge::forTarget(&api, QStringLiteral("sub_own_drop_target"));
    ASSERT_NE(bridge, nullptr);

    logos::SubHandle first = subscribeOn(bridge, "one", &api);
    logos::SubHandle second = subscribeOn(bridge, "two", &api);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);

    EXPECT_EQ(logos::qt::dropSubscriptions(&api), 2u);
    EXPECT_FALSE(first);
    EXPECT_FALSE(second);
}

// Idempotent, because the callers are a host tearing a mount down and a view
// backend's own aboutToUnload() — and on a clean teardown BOTH run.
TEST_F(LpSubscriptionOwnershipTest, DroppingTwiceIsNotAnError)
{
    LogosAPI api(QStringLiteral("sub_own_twice"));
    logos::qt::LpBridge* bridge =
        logos::qt::LpBridge::forTarget(&api, QStringLiteral("sub_own_twice_target"));
    ASSERT_TRUE(subscribeOn(bridge, "once", &api));

    EXPECT_EQ(logos::qt::dropSubscriptions(&api), 1u);
    EXPECT_EQ(logos::qt::dropSubscriptions(&api), 0u);
}

// One mount closing must not silence another. Two identities of the SAME module
// name share one bridge (it is keyed by (origin, target)), so this is not a
// question about bridges: it is about whether the owner is really consulted.
TEST_F(LpSubscriptionOwnershipTest, DroppingOneIdentityLeavesAnothersAlone)
{
    LogosAPI mine(QStringLiteral("sub_own_shared"));
    LogosAPI theirs(QStringLiteral("sub_own_shared"));
    logos::qt::LpBridge* one =
        logos::qt::LpBridge::forTarget(&mine, QStringLiteral("sub_own_shared_target"));
    logos::qt::LpBridge* two =
        logos::qt::LpBridge::forTarget(&theirs, QStringLiteral("sub_own_shared_target"));
    ASSERT_EQ(one, two) << "same (origin, target) pair, so the same bridge";

    logos::SubHandle ours = subscribeOn(one, "ours", &mine);
    logos::SubHandle hers = subscribeOn(two, "theirs", &theirs);
    ASSERT_TRUE(ours);
    ASSERT_TRUE(hers);

    EXPECT_EQ(logos::qt::dropSubscriptions(&mine), 1u);
    EXPECT_FALSE(ours);
    EXPECT_TRUE(hers) << "the other mount is still open and still listening";

    EXPECT_EQ(logos::qt::dropSubscriptions(&theirs), 1u);
    EXPECT_FALSE(hers);
}

// A subscription with NO owner belongs to the process, which is what every
// module-lifetime consumer has always had and must keep: a `bind_x(...)`
// temporary's subscription outliving the temporary is the contract the whole
// bridge exists for.
TEST_F(LpSubscriptionOwnershipTest, AProcessOwnedSubscriptionBelongsToNoIdentity)
{
    LogosAPI api(QStringLiteral("sub_own_process"));
    logos::qt::LpBridge* bridge =
        logos::qt::LpBridge::forTarget(&api, QStringLiteral("sub_own_process_target"));
    logos::SubHandle handle = logos::qt::subscribe(bridge, "forever",
                                                   [](nlohmann::json) {});
    ASSERT_TRUE(handle);

    EXPECT_EQ(logos::qt::dropSubscriptions(&api), 0u);
    EXPECT_TRUE(handle);
}

// ── the floor: a destroyed identity takes its subscriptions with it ─────────
//
// Everything above is a call someone has to make. This is the one that needs no
// caller, and it is the reason four shipped `ui_qml` modules do not have to be
// patched one at a time: the mount's LogosAPI announces its own death and the
// bridge acts on it. A fifth view module written next week gets this without
// its author knowing the seam exists.
TEST_F(LpSubscriptionOwnershipTest, ADestroyedIdentityUnArmsWhatItTook)
{
    logos::SubHandle survivor;
    {
        LogosAPI mount(QStringLiteral("sub_own_mount"));
        logos::qt::LpBridge* bridge =
            logos::qt::LpBridge::forTarget(&mount, QStringLiteral("sub_own_mount_target"));
        ASSERT_NE(bridge, nullptr);
        survivor = subscribeOn(bridge, "delivery_state_changed", &mount);
        ASSERT_TRUE(survivor);
    }

    EXPECT_FALSE(survivor)
        << "the mount is gone; a callback holding its freed backend must not "
           "still be armed";
}

// ...and the SECOND mount is unaffected by the first one's death, which is the
// close-and-reopen the crash was found on: open, close, open again.
TEST_F(LpSubscriptionOwnershipTest, AReopenedMountKeepsItsOwnSubscriptions)
{
    logos::SubHandle firstMount;
    {
        LogosAPI mount(QStringLiteral("sub_own_reopen"));
        firstMount = subscribeOn(
            logos::qt::LpBridge::forTarget(&mount, QStringLiteral("sub_own_reopen_target")),
            "message_received", &mount);
        ASSERT_TRUE(firstMount);
    }
    ASSERT_FALSE(firstMount);

    LogosAPI remount(QStringLiteral("sub_own_reopen"));
    logos::SubHandle secondMount = subscribeOn(
        logos::qt::LpBridge::forTarget(&remount, QStringLiteral("sub_own_reopen_target")),
        "message_received", &remount);
    EXPECT_TRUE(secondMount);
    EXPECT_FALSE(firstMount) << "the first mount's callback stays dead";
}

// A null bridge — the forTarget path with a null LogosAPI, which could never
// work — yields an empty handle rather than a crash or a lie.
TEST_F(LpSubscriptionOwnershipTest, ANullBridgeSubscribesToNothing)
{
    LogosAPI api(QStringLiteral("sub_own_null"));
    EXPECT_FALSE(subscribeOn(nullptr, "nothing", &api));
}

// Dropping "nobody's" subscriptions is refused rather than interpreted. Null is
// how a process-owned subscription is spelled, so matching it would make
// `dropSubscriptions(someNullPointer)` silence every module-lifetime consumer
// in the image.
TEST_F(LpSubscriptionOwnershipTest, DroppingANullOwnerDropsNothing)
{
    LogosAPI api(QStringLiteral("sub_own_null_owner"));
    logos::qt::LpBridge* bridge =
        logos::qt::LpBridge::forTarget(&api, QStringLiteral("sub_own_null_owner_target"));
    ASSERT_TRUE(logos::qt::subscribe(bridge, "forever", [](nlohmann::json) {}));

    EXPECT_EQ(logos::qt::dropSubscriptions(nullptr), 0u);
}

}  // namespace
