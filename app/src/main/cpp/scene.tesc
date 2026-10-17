#version 450

layout(vertices = 3) out;

layout(push_constant) uniform SceneParams {
    float time;
    float aspect;
    float object_count;
    float scene_mode;
    float rotation;
} params;

layout(location = 0) in vec3 in_color[];
layout(location = 0) out vec3 out_color[];
layout(location = 1) in vec4 in_pattern_data[];
layout(location = 1) out vec4 out_pattern_data[];

void main() {
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    out_color[gl_InvocationID] = in_color[gl_InvocationID];
    out_pattern_data[gl_InvocationID] = in_pattern_data[gl_InvocationID];

    if (gl_InvocationID == 0) {
        float level = clamp(params.object_count, 2.0, 32.0);
        gl_TessLevelOuter[0] = level;
        gl_TessLevelOuter[1] = level;
        gl_TessLevelOuter[2] = level;
        gl_TessLevelInner[0] = level;
    }
}
