// SPDX-License-Identifier: Apache-2.0
// Clean-room native oracle fixture for FrostCommon.natvis, without third-party implementations.
// Build with the existing MSVC debuggee provisioning; compare DAP children to cdb dx for this
// same image at visualizer_breakpoint. No expected summary is fabricated by the IDE.
struct ImVec2 { float x, y; };
struct ImVec4 { float x, y, z, w; };
namespace glm {
template<int N, class T, int Q> struct vec;
template<class T, int Q> struct vec<3,T,Q> { T x,y,z; };
template<class T, int Q> struct qua { T w,x,y,z; };
}
ImVec2 point{2,7}; ImVec4 color{1,2,3,4};
glm::vec<3,float,0> vector{3,5,8}; glm::qua<float,0> rotation{1,0,0,0};
extern "C" __declspec(noinline) void visualizer_breakpoint() { volatile int stop = 1; (void)stop; }
int main() {
    visualizer_breakpoint();
    return int(point.x + color.y + vector.z + rotation.w) == 13 ? 0 : 1;
}
