// Synthetic Vulkan game (tests only): a flat colour from push constants.
#version 450
layout(push_constant) uniform PC { layout(offset = 0) vec4 color; } pc;
layout(location = 0) out vec4 o;
void main() { o = pc.color; }
