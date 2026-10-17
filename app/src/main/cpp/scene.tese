#version 450

layout(triangles, equal_spacing, ccw) in;

layout(push_constant) uniform SceneParams {
    float time;
    float aspect;
    float object_count;
    float scene_mode;
    float rotation;
} params;

layout(location = 0) in vec3 in_color[];
layout(location = 0) out vec3 face_color;
layout(location = 1) in vec4 in_pattern_data[];
layout(location = 1) out vec4 pattern_data;

void apply_touch_rotation() {
    float c = cos(params.rotation);
    float s = sin(params.rotation);
    gl_Position.xy = mat2(c, -s, s, c) * gl_Position.xy;
}

void main() {
    pattern_data = vec4(0.0, 0.0, -1.0, 0.0);
    vec3 p = gl_TessCoord.x * gl_in[0].gl_Position.xyz +
             gl_TessCoord.y * gl_in[1].gl_Position.xyz +
             gl_TessCoord.z * gl_in[2].gl_Position.xyz;
    vec3 color = gl_TessCoord.x * in_color[0] +
                 gl_TessCoord.y * in_color[1] +
                 gl_TessCoord.z * in_color[2];

    vec2 centered = p.xy - vec2(0.0, -0.25);
    float radius = length(centered);
    vec2 direction = radius > 0.00001 ? centered / radius : vec2(0.0, 1.0);
    float boundary = 100.0;
    float base_projection = -direction.y;
    float right_projection = 0.8660254 * direction.x + 0.5 * direction.y;
    float left_projection = -0.8660254 * direction.x + 0.5 * direction.y;
    if (base_projection > 0.00001)
        boundary = min(boundary, 1.35 / base_projection);
    if (right_projection > 0.00001)
        boundary = min(boundary, 1.35 / right_projection);
    if (left_projection > 0.00001)
        boundary = min(boundary, 1.35 / left_projection);
    vec2 disk = direction * clamp(radius / boundary, 0.0, 1.0);
    float dome = sqrt(max(0.0, 1.0 - dot(disk, disk)));

    if (params.scene_mode > 3.5) {
        vec2 shadow = vec2(disk.x * 0.78, disk.y * 0.18 - 0.58);
        gl_Position = vec4(shadow.x * 0.58 / max(params.aspect, 0.5),
                           shadow.y * 0.58, 0.96, 1.0);
        face_color = vec3(0.025, 0.035, 0.055);
        pattern_data = vec4(disk * 0.5 + 0.5, 13.0, length(disk));
        apply_touch_rotation();
        return;
    }

    float yaw = params.time * 0.24 + params.rotation;
    vec2 rotated_xz = mat2(cos(yaw), -sin(yaw), sin(yaw), cos(yaw)) *
                      vec2(disk.x, dome * 0.88);
    vec3 normal = normalize(vec3(rotated_xz.x, disk.y, rotated_xz.y));
    vec3 light = normalize(vec3(-0.45, 0.68, 0.72));
    float diffuse = max(dot(normal, light), 0.0);
    float shade = 0.20 + diffuse * 0.80;
    float specular = pow(max(dot(reflect(-light, normal), vec3(0.0, 0.0, 1.0)),
                             0.0), 20.0);
    vec2 surface = vec2(rotated_xz.x, disk.y);
    gl_Position = vec4(
        surface.x * 0.62 / max(params.aspect, 0.5),
        surface.y * 0.62 + 0.08,
        0.64 - rotated_xz.y * 0.16,
        1.0
    );
    face_color = color * shade + vec3(specular * 0.72);
    pattern_data = vec4(disk * 0.5 + 0.5, 12.0, length(disk));
    apply_touch_rotation();
}
