// SPDX-License-Identifier: Apache-2.0
// Tests detached signatures and payload verification without network or release keys.
#include <frost/update/manifest.hpp>
#include <frost/update/verification.hpp>
#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

#include <sodium.h>

namespace {
TEST(UpdateFloor, ExactMaximumVersionAndBuildRoundTripAndStrictValidation) {
    frost::update::ReleaseFloor floor{"1.2.3+" + std::string(122, 'x'), std::numeric_limits<std::uint64_t>::max(),
                                      std::string(64, 'a')};
    ASSERT_EQ(floor.version.size(), 128U);
    auto encoded = frost::update::encodeReleaseFloor(floor);
    ASSERT_TRUE(encoded);
    auto parsed = frost::update::parseReleaseFloor(*encoded);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->version, floor.version);
    EXPECT_EQ(parsed->build, floor.build);
    EXPECT_EQ(parsed->digest, floor.digest);
    auto duplicate = *encoded;
    duplicate.insert(1, "\"schema\":1,");
    EXPECT_FALSE(frost::update::parseReleaseFloor(duplicate));
    EXPECT_FALSE(frost::update::parseReleaseFloor(""));
    EXPECT_FALSE(frost::update::parseReleaseFloor(std::string(1025, 'x')));
    auto overflow = *encoded;
    overflow.replace(overflow.find("18446744073709551615"), 20, "18446744073709551616");
    EXPECT_FALSE(frost::update::parseReleaseFloor(overflow));
    floor.digest = std::string(64, 'A');
    EXPECT_FALSE(frost::update::encodeReleaseFloor(floor));
    floor.digest = std::string(64, 'a');
    floor.build = 0;
    EXPECT_FALSE(frost::update::encodeReleaseFloor(floor));
}
std::span<const std::byte> bytes(const std::string& s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}
TEST(UpdateVerification, SignatureBindsExactBytesAndKey) {
    ASSERT_GE(sodium_init(), 0);
    frost::update::PublicKey key{};
    std::array<unsigned char, crypto_sign_SECRETKEYBYTES> secret{};
    ASSERT_EQ(crypto_sign_keypair(key.data(), secret.data()), 0);
    const std::string manifest = R"({"version":"1.2.3"})";
    frost::update::Signature signature{};
    ASSERT_EQ(crypto_sign_detached(signature.data(), nullptr, reinterpret_cast<const unsigned char*>(manifest.data()),
                                   manifest.size(), secret.data()),
              0);
    EXPECT_TRUE(frost::update::verifyManifest(bytes(manifest), signature, key));
    EXPECT_FALSE(frost::update::verifyManifest(bytes(manifest + " "), signature, key));
    key[0] ^= 1;
    EXPECT_FALSE(frost::update::verifyManifest(bytes(manifest), signature, key));
    EXPECT_FALSE(frost::update::verifyManifest({}, signature, key));
    std::string oversized(65537, 'x');
    EXPECT_FALSE(frost::update::verifyManifest(bytes(oversized), signature, key));
    sodium_memzero(secret.data(), secret.size());
}
TEST(UpdateVerification, PayloadMustMatchBothLengthAndDigest) {
    ASSERT_GE(sodium_init(), 0);
    const std::string payload = "fixture payload";
    frost::update::Sha256 digest{};
    crypto_hash_sha256(digest.data(), reinterpret_cast<const unsigned char*>(payload.data()), payload.size());
    EXPECT_TRUE(frost::update::verifyPayload(bytes(payload), payload.size(), digest));
    EXPECT_FALSE(frost::update::verifyPayload(bytes(payload), payload.size() + 1, digest));
    EXPECT_FALSE(frost::update::verifyPayload(bytes(payload + "!"), payload.size() + 1, digest));
}
TEST(UpdateManifest, SignedPolicyRejectsReplayWrongHostAndDuplicateFields) {
    ASSERT_GE(sodium_init(), 0);
    frost::update::PublicKey key{};
    std::array<unsigned char, crypto_sign_SECRETKEYBYTES> secret{};
    ASSERT_EQ(crypto_sign_keypair(key.data(), secret.data()), 0);
    auto verify = [&](const std::string& json, frost::update::ReleasePolicy policy) {
        frost::update::Signature signature{};
        EXPECT_EQ(crypto_sign_detached(signature.data(), nullptr, reinterpret_cast<const unsigned char*>(json.data()),
                                       json.size(), secret.data()),
                  0);
        return frost::update::verifyRelease(bytes(json), signature, key, policy);
    };
    const std::string json =
        R"({"schema":1,"channel":"stable","platform":"windows","arch":"x64","version":"1.2.3","build":42,"expires":2000,"asset":{"url":"https://github.com/Frowsty/FrostIDE/releases/download/v1.2.3/Frost.msi","size":15,"sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}})";
    frost::update::ReleasePolicy policy{"stable", "windows", "x64", 1000, 40, 41, "1.2.2", "1.2.2", ""};
    ASSERT_TRUE(verify(json, policy));
    policy.acceptedBuild = 42;
    EXPECT_FALSE(verify(json, policy));
    policy.acceptedVersion = "1.2.3";
    policy.acceptedDigest = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    EXPECT_TRUE(verify(json, policy));
    policy.acceptedDigest = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    EXPECT_FALSE(verify(json, policy));
    policy.acceptedBuild = 41;
    policy.acceptedVersion = "1.2.2";
    policy.acceptedDigest = "";
    policy.channel = "eap";
    EXPECT_FALSE(verify(json, policy));
    policy.channel = "stable";
    auto evil = json;
    evil.replace(evil.find("github.com"), 10, "evil.testx");
    EXPECT_FALSE(verify(evil, policy));
    auto duplicate = json;
    duplicate.insert(1, "\"schema\":1,");
    EXPECT_FALSE(verify(duplicate, policy));
    sodium_memzero(secret.data(), secret.size());
}
TEST(UpdateManifest, SemVerPrecedenceDoesNotOverflowOrPermitChannelDowngrades) {
    ASSERT_GE(sodium_init(), 0);
    frost::update::PublicKey key{};
    std::array<unsigned char, crypto_sign_SECRETKEYBYTES> secret{};
    ASSERT_EQ(crypto_sign_keypair(key.data(), secret.data()), 0);
    auto verify = [&](std::string_view candidate, std::string_view previous, std::string_view channel) {
        const auto json = "{\"schema\":1,\"channel\":\"" + std::string(channel) +
                          "\",\"platform\":\"windows\",\"arch\":\"x64\",\"version\":\"" + std::string(candidate) +
                          "\",\"build\":43,\"expires\":2000,\"asset\":{\"url\":\"https://github.com/Frowsty/FrostIDE/"
                          "releases/download/v1/Frost.msi\",\"size\":7,\"sha256\":\"" +
                          std::string(64, 'a') + "\"}}";
        frost::update::Signature signature{};
        EXPECT_EQ(crypto_sign_detached(signature.data(), nullptr, reinterpret_cast<const unsigned char*>(json.data()),
                                       static_cast<unsigned long long>(json.size()), secret.data()),
                  0);
        const std::string digest(64, 'a');
        frost::update::ReleasePolicy policy{channel, "windows", "x64", 1000, 40, 42, previous, previous, digest};
        return frost::update::verifyRelease(bytes(json), signature, key, policy).has_value();
    };
    const std::array<std::string_view, 8> versions{"1.2.3-alpha",  "1.2.3-alpha.1", "1.2.3-alpha.beta", "1.2.3-beta",
                                                   "1.2.3-beta.2", "1.2.3-beta.11", "1.2.3-rc.1",       "1.2.3"};
    for (std::size_t i = 1; i < versions.size(); ++i) {
        EXPECT_TRUE(verify(versions[i], versions[i - 1], "eap"));
        EXPECT_FALSE(verify(versions[i - 1], versions[i], "eap"));
    }
    EXPECT_TRUE(verify("18446744073709551616.0.0", "18446744073709551615.0.0", "stable"));
    EXPECT_FALSE(verify("1.2.3-01", "1.2.2", "eap"));
    EXPECT_FALSE(verify("1.2.3-eap.1", "1.2.2", "stable"));
    EXPECT_FALSE(verify("1.2.3", "1.3.0-eap.1", "stable"));
    EXPECT_TRUE(verify("1.2.3+build.2", "1.2.3+build.1", "stable"));
    sodium_memzero(secret.data(), secret.size());
}
} // namespace
