// Synthetic Vulkan game (tests only): one big triangle per draw, moved by the
// camera block (set 0 binding 0, the ViewProjection at byte 64, its inverse at
// byte 192) and a per-object block (set 0 binding 1, dynamic offset).
// Regenerate the header: scripts/gen-vk-shaders.sh
#version 450
layout(set = 0, binding = 0, std140) uniform Camera {
    vec4 junk[4];
    mat4 vp;
    mat4 other;
    mat4 inv_vp;
} cam;
layout(set = 0, binding = 1, std140) uniform Object { vec4 offset; } obj;

void main()
{
    const vec2 p[3] = vec2[3](vec2(-40.0, -40.0), vec2(40.0, -40.0), vec2(0.0, 60.0));
    const vec3 world = vec3(p[gl_VertexIndex], 5.0) + obj.offset.xyz;
    gl_Position = cam.vp * vec4(world, 1.0);
}
