#pragma once

#ifndef TINY_DEBUG
#error "TINY_DEBUG comes from the Debug|x64 ItemDefinitionGroup"
#endif

inline int tinyValue() { return TINY_DEBUG; }
