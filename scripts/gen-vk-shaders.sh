#!/bin/sh
# Rebuilds the SPIR-V headers of the Vulkan layer's shaders (src/host_vk/shaders).
# Needs glslangValidator (Vulkan SDK, or `apt install glslang-tools`). The
# headers are committed so the Windows build needs no shader compiler.
set -e
cd "$(dirname "$0")/../src/host_vk/shaders"
glslangValidator -V --target-env vulkan1.0 --vn kCameraMotionSpv -o camera_motion_spv.h camera_motion.comp
glslangValidator -V --target-env vulkan1.0 --vn kCopyTestSpv -o copy_test_spv.h copy_test.comp
cd ../../../tests/host_vk/shaders
glslangValidator -V --target-env vulkan1.0 --vn kGameVertSpv -o game_vert_spv.h game.vert
glslangValidator -V --target-env vulkan1.0 --vn kGameFragSpv -o game_frag_spv.h game.frag
