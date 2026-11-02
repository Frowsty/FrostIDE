#pragma once

#if !defined(FROST_SDK_FEATURE)
#error "FROST_SDK_FEATURE comes from Sdk.props"
#endif

inline int sdkValue() { return FROST_SDK_FEATURE; }
