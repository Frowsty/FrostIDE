// SPDX-License-Identifier: Apache-2.0
//
// toolchain_probe fixture: #embed is banned (PLAN.md section 2.1). Must not compile under our flags.
static const char kData[] = {
#embed "embed.cpp"
};
int probeEmbed() { return kData[0]; }
