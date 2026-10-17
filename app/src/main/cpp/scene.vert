#version 450

layout(push_constant) uniform SceneParams {
    float time;
    float aspect;
    float object_count;
    float scene_mode;
    float rotation;
} params;

layout(location = 0) out vec3 face_color;
layout(location = 1) out vec4 pattern_data;

const vec3 cube_vertices[36] = vec3[](
    vec3(-1,-1,-1), vec3(-1, 1,-1), vec3( 1, 1,-1),
    vec3(-1,-1,-1), vec3( 1, 1,-1), vec3( 1,-1,-1),
    vec3(-1,-1, 1), vec3( 1,-1, 1), vec3( 1, 1, 1),
    vec3(-1,-1, 1), vec3( 1, 1, 1), vec3(-1, 1, 1),
    vec3(-1,-1,-1), vec3(-1,-1, 1), vec3(-1, 1, 1),
    vec3(-1,-1,-1), vec3(-1, 1, 1), vec3(-1, 1,-1),
    vec3( 1,-1,-1), vec3( 1, 1,-1), vec3( 1, 1, 1),
    vec3( 1,-1,-1), vec3( 1, 1, 1), vec3( 1,-1, 1),
    vec3(-1, 1,-1), vec3(-1, 1, 1), vec3( 1, 1, 1),
    vec3(-1, 1,-1), vec3( 1, 1, 1), vec3( 1, 1,-1),
    vec3(-1,-1,-1), vec3( 1,-1,-1), vec3( 1,-1, 1),
    vec3(-1,-1,-1), vec3( 1,-1, 1), vec3(-1,-1, 1)
);

const vec3 palette[6] = vec3[](
    vec3(0.10, 0.72, 0.92), vec3(0.18, 0.88, 0.62),
    vec3(0.97, 0.46, 0.30), vec3(0.72, 0.36, 0.96),
    vec3(0.98, 0.74, 0.22), vec3(0.26, 0.48, 0.98)
);

void apply_touch_rotation() {
    float c = cos(params.rotation);
    float s = sin(params.rotation);
    vec2 ndc = gl_Position.xy / gl_Position.w;
    gl_Position.xy = mat2(c, -s, s, c) * ndc * gl_Position.w;
}

vec3 dolphin_body_point(uint ring, uint sector) {
    float phi = 3.14159265 * float(ring) / 6.0;
    float theta = 6.28318530 * float(sector) / 12.0;
    float radius = sin(phi);
    return vec3(1.05 * cos(phi),
                0.42 * radius * cos(theta),
                0.34 * radius * sin(theta));
}

vec3 dolphin_vertex(uint vertex) {
    if (vertex < 432u) {
        uint triangle = vertex / 3u;
        uint corner = vertex % 3u;
        uint cell = triangle / 2u;
        uint tri_half = triangle % 2u;
        uint ring = cell / 12u;
        uint sector = cell % 12u;
        uint ring0 = ring;
        uint ring1 = ring + 1u;
        uint sector0 = sector;
        uint sector1 = (sector + 1u) % 12u;
        if (tri_half == 0u) {
            if (corner == 0u) return dolphin_body_point(ring0, sector0);
            if (corner == 1u) return dolphin_body_point(ring1, sector0);
            return dolphin_body_point(ring1, sector1);
        }
        if (corner == 0u) return dolphin_body_point(ring0, sector0);
        if (corner == 1u) return dolphin_body_point(ring1, sector1);
        return dolphin_body_point(ring0, sector1);
    }

    if (vertex < 456u) {
        uint part = vertex - 432u;
        uint side = part / 12u;
        uint corner = part % 3u;
        uint tail_triangle = (part / 3u) % 4u;
        float sign_y = side == 0u ? 1.0 : -1.0;
        vec3 root = vec3(-0.78, 0.0, 0.0);
        vec3 inner = vec3(-0.98, sign_y * 0.12, 0.0);
        vec3 tip = vec3(-1.52, sign_y * 0.43, 0.0);
        vec3 mid = vec3(-1.24, sign_y * 0.18, 0.0);
        if (tail_triangle < 2u) {
            if (corner == 0u) return root;
            if (corner == 1u) return inner;
            return mid;
        }
        if (corner == 0u) return inner;
        if (corner == 1u) return tip;
        return mid;
    }

    if (vertex < 492u) {
        uint part = vertex - 456u;
        uint sector = part / 3u;
        uint corner = part % 3u;
        float theta = 6.28318530 * float(sector) / 12.0;
        if (corner == 0u) return vec3(0.88, 0.11 * cos(theta), 0.11 * sin(theta));
        if (corner == 1u) {
            float next_theta = 6.28318530 * float(sector + 1u) / 12.0;
            return vec3(0.88, 0.11 * cos(next_theta), 0.11 * sin(next_theta));
        }
        return vec3(1.62, 0.0, 0.0);
    }

    if (vertex < 495u) {
        uint corner = vertex - 492u;
        if (corner == 0u) return vec3(-0.30, 0.34, 0.0);
        if (corner == 1u) return vec3(0.28, 0.28, 0.0);
        return vec3(0.02, 0.78, 0.0);
    }

    uint side = (vertex - 495u) / 3u;
    uint corner = vertex % 3u;
    float sign_y = side == 0u ? 1.0 : -1.0;
    if (corner == 0u) return vec3(0.10, -0.16, 0.0);
    if (corner == 1u) return vec3(-0.28, -0.34, 0.02);
    return vec3(0.48, -0.30, sign_y * 0.34);
}

void main() {
    uint vertex = uint(gl_VertexIndex);
    uint instance = uint(gl_InstanceIndex);
    float scene_mode = params.scene_mode;
    pattern_data = vec4(0.0, 0.0, -1.0, 0.0);

    if (scene_mode > 16.5) {
        uint triangle = vertex / 3u;
        uint corner = vertex % 3u;
        vec2 shape = corner == 0u ? vec2(-0.84, -0.68) :
                     corner == 1u ? vec2( 0.84, -0.68) : vec2(0.0, 0.92);
        float columns = 256.0;
        float rows = max(1.0, ceil(params.object_count / columns));
        float col = mod(float(triangle), columns);
        float row = floor(float(triangle) / columns);
        float grid_y = params.aspect >= 1.0 ? 0.90 : 0.90 * params.aspect;
        float grid_x = grid_y / max(params.aspect, 0.1);
        vec2 center = vec2(
            ((col + 0.5) * 2.0 / columns - 1.0) * grid_x,
            (1.0 - (row + 0.5) * 2.0 / rows) * grid_y
        );
        vec2 half_cell = vec2(grid_x / columns, grid_y / rows) * 0.47;
        float angle = params.time * 0.18 + float(triangle % 19u) * 0.025;
        shape = mat2(cos(angle), -sin(angle), sin(angle), cos(angle)) * shape;
        gl_Position = vec4(
            center.x + shape.x * half_cell.x,
            center.y + shape.y * half_cell.y,
            0.38 + float(triangle % 7u) * 0.001, 1.0
        );
        face_color = palette[triangle % 6u] *
                     (0.76 + 0.24 * sin(params.time + float(triangle) * 0.009));
        apply_touch_rotation();
        return;
    }

    if (scene_mode > 15.5) {
        vec2 shape = vertex % 3u == 0u ? vec2(-0.78, -0.65) :
                     vertex % 3u == 1u ? vec2( 0.78, -0.65) : vec2(0.0, 0.88);
        float columns = 32.0;
        float rows = max(1.0, ceil(params.object_count / columns));
        float col = mod(float(instance), columns);
        float row = floor(float(instance) / columns);
        float grid_y = params.aspect >= 1.0 ? 0.88 : 0.88 * params.aspect;
        float grid_x = grid_y / max(params.aspect, 0.1);
        vec2 center = vec2(
            ((col + 0.5) * 2.0 / columns - 1.0) * grid_x,
            (1.0 - (row + 0.5) * 2.0 / rows) * grid_y
        );
        vec2 half_cell = vec2(grid_x / columns, grid_y / rows) * 0.42;
        float phase = params.time * 0.55 + float(instance) * 0.037;
        shape = mat2(cos(phase), -sin(phase), sin(phase), cos(phase)) * shape;
        gl_Position = vec4(
            center.x + shape.x * half_cell.x,
            center.y + shape.y * half_cell.y,
            0.38 + float(instance % 9u) * 0.001, 1.0
        );
        face_color = palette[instance % 6u] *
                     (0.70 + 0.30 * sin(phase + float(vertex)));
        apply_touch_rotation();
        return;
    }

    if (scene_mode > 14.5) {
        vec2 corner = vertex == 0u ? vec2(-1.0, -1.0) :
                      vertex == 1u ? vec2( 1.0, -1.0) :
                      vertex == 2u ? vec2( 1.0,  1.0) :
                      vertex == 3u ? vec2(-1.0, -1.0) :
                      vertex == 4u ? vec2( 1.0,  1.0) :
                                     vec2(-1.0,  1.0);
        float t = params.time * 0.6 + float(instance) * 0.17;
        vec2 center = vec2(sin(t) * 0.12, cos(t * 0.73) * 0.10);
        vec2 uv = corner * 0.5 + 0.5;
        gl_Position = vec4((center.x + corner.x * 0.90) /
                               max(params.aspect, 0.5),
                           center.y + corner.y * 0.90,
                           0.30 + float(instance) * 0.001, 1.0);
        face_color = palette[instance % 6u];
        pattern_data = vec4(uv, 15.0, params.time + float(instance) * 0.31);
        apply_touch_rotation();
        return;
    }

    if (scene_mode > 13.5) {
        vec2 corner = vertex == 0u ? vec2(-1.0, -1.0) :
                      vertex == 1u ? vec2( 1.0, -1.0) :
                      vertex == 2u ? vec2( 1.0,  1.0) :
                      vertex == 3u ? vec2(-1.0, -1.0) :
                      vertex == 4u ? vec2( 1.0,  1.0) :
                                     vec2(-1.0,  1.0);
        float phase = params.time * 1.3 + float(instance) * 0.29;
        vec2 offset = vec2(sin(phase) * 0.10, cos(phase * 0.8) * 0.08);
        vec2 uv = corner * 0.5 + 0.5;
        gl_Position = vec4((offset.x + corner.x * 0.92) /
                               max(params.aspect, 0.5),
                           offset.y + corner.y * 0.92,
                           0.35, 1.0);
        face_color = palette[instance % 6u];
        pattern_data = vec4(uv, 14.0, params.time + float(instance) * 0.23);
        apply_touch_rotation();
        return;
    }

    if (scene_mode > 12.5) {
        vec2 point = vertex == 0u ? vec2(-0.70, -0.60) :
                     vertex == 1u ? vec2( 0.70, -0.60) : vec2(0.0, 0.72);
        float phase = params.time * 0.8 + float(instance) * 0.07;
        float sx = 0.58 + 0.16 * sin(phase * 1.1);
        float sy = 0.55 + 0.18 * cos(phase * 0.9);
        float angle = phase + sin(phase * 0.37) * 0.4;
        for (int i = 0; i < 4; ++i) {
            float fi = float(i) + 1.0;
            angle += sin(phase * fi + float(vertex) * 0.21) * 0.07;
            sx *= 0.98 + 0.02 * cos(phase * fi);
            sy *= 0.98 + 0.02 * sin(phase * fi);
        }
        point = mat2(cos(angle), -sin(angle), sin(angle), cos(angle)) *
                vec2(point.x * sx, point.y * sy);
        float columns = 32.0;
        float rows = max(1.0, ceil(params.object_count / columns));
        vec2 center = vec2(
            (mod(float(instance), columns) + 0.5) * 2.0 / columns - 1.0,
            1.0 - (floor(float(instance) / columns) + 0.5) * 2.0 / rows
        );
        gl_Position = vec4((center.x + point.x * 0.11) /
                               max(params.aspect, 0.5),
                           center.y + point.y * 0.12, 0.38, 1.0);
        face_color = palette[instance % 6u] *
                     (0.74 + 0.26 * sin(phase + float(vertex)));
        apply_touch_rotation();
        return;
    }

    if (scene_mode > 11.5) {
        vec2 point = vertex == 0u ? vec2(-0.5, -0.42) :
                     vertex == 1u ? vec2(0.5, -0.42) : vec2(0.0, 0.54);
        float columns = 32.0;
        float rows = max(1.0, ceil(params.object_count / columns));
        float col = mod(float(instance), columns);
        float row = floor(float(instance) / columns);
        float scale = 0.19 + 0.045 * sin(params.time * 1.7 + float(instance) * 0.09);
        float angle = params.time * (0.8 + mod(float(instance), 5.0) * 0.12);
        point *= scale;
        point = mat2(cos(angle), -sin(angle), sin(angle), cos(angle)) * point;
        vec2 center = vec2(
            (col + 0.5) * 2.0 / columns - 1.0,
            1.0 - (row + 0.5) * 2.0 / rows
        );
        gl_Position = vec4((center.x + point.x) / max(params.aspect, 0.5),
                           center.y + point.y, 0.42, 1.0);
        face_color = palette[instance % 6u] *
                     (0.72 + 0.28 * sin(params.time * 2.1 + float(instance) * 0.04));
        apply_touch_rotation();
        return;
    }

    if (scene_mode > 10.5) {
        vec2 uv = vertex == 0u ? vec2(-1.0, -1.0) :
                  vertex == 1u ? vec2( 1.0, -1.0) :
                  vertex == 2u ? vec2( 1.0,  1.0) :
                  vertex == 3u ? vec2(-1.0, -1.0) :
                  vertex == 4u ? vec2( 1.0,  1.0) :
                                 vec2(-1.0,  1.0);
        float angle = params.time * 0.24 + float(instance) * 0.17;
        vec2 center = vec2(sin(angle) * 0.18, cos(angle * 0.73) * 0.12);
        gl_Position = vec4((center.x + uv.x * 0.78) / max(params.aspect, 0.5),
                           center.y + uv.y * 0.78, 0.35 + float(instance) * 0.001, 1.0);
        face_color = vec3(1.0);
        pattern_data = vec4(uv * 0.5 + 0.5, 11.0,
                            params.time + float(instance) * 0.31);
        apply_touch_rotation();
        return;
    }

    if (scene_mode > 9.5) {
        vec2 point = vertex == 0u ? vec2(-0.58, -0.46) :
                     vertex == 1u ? vec2( 0.58, -0.46) : vec2(0.0, 0.62);
        float col = mod(float(instance), 8.0);
        float row = floor(float(instance) / 8.0);
        float angle = params.time * (0.45 + float(instance % 9u) * 0.04) +
                      float(instance) * 0.39;
        point = mat2(cos(angle), -sin(angle), sin(angle), cos(angle)) * point;
        vec2 center = vec2((col - 3.5) * 0.20, (row - 3.5) * 0.16);
        gl_Position = vec4((center.x + point.x * 0.56) / max(params.aspect, 0.5),
                           center.y + point.y * 0.56, 0.42, 1.0);
        face_color = palette[instance % 6u];
        pattern_data = vec4(0.0, 0.0, 10.0,
                            0.24 + 0.26 * (0.5 + 0.5 *
                                sin(params.time * 1.8 + float(instance) * 0.21)));
        apply_touch_rotation();
        return;
    }

    if (scene_mode > 8.5) {
        vec2 point = vertex == 0u ? vec2(-0.62, -0.50) :
                     vertex == 1u ? vec2( 0.62, -0.50) : vec2(0.0, 0.66);
        float angle = params.time * (0.5 + float(instance % 7u) * 0.11) +
                      float(instance) * 0.47;
        point = mat2(cos(angle), -sin(angle), sin(angle), cos(angle)) * point;
        float depth = 0.12 + 0.76 *
            (float(instance) + 0.5) / max(params.object_count, 1.0);
        float scale = 1.0 - 0.42 *
            (float(instance) / max(params.object_count, 1.0));
        gl_Position = vec4(point.x * scale / max(params.aspect, 0.5),
                           point.y * scale, depth, 1.0);
        face_color = palette[instance % 6u] *
                     (0.74 + 0.26 * sin(params.time + float(instance) * 0.2));
        apply_touch_rotation();
        return;
    }

    if (scene_mode > 7.5) {
        vec2 corner = vertex == 0u ? vec2(-1.0, -1.0) :
                      vertex == 1u ? vec2( 1.0, -1.0) :
                      vertex == 2u ? vec2( 1.0,  1.0) :
                      vertex == 3u ? vec2(-1.0, -1.0) :
                      vertex == 4u ? vec2( 1.0,  1.0) :
                                     vec2(-1.0,  1.0);
        float columns = 16.0;
        float rows = 16.0;
        float col = mod(float(instance), columns);
        float row = floor(float(instance) / columns);
        vec2 uv = corner * 0.5 + 0.5;
        float format_index = floor(row * 10.0 / rows);
        float aspect = max(params.aspect, 0.1);
        float grid_y = aspect >= 1.0 ? 0.90 : 0.90 * aspect;
        float grid_x = grid_y / aspect;
        vec2 center = vec2(
            ((col + 0.5) * 2.0 / columns - 1.0) * grid_x,
            (1.0 - (row + 0.5) * 2.0 / rows) * grid_y
        );
        vec2 half_size = vec2(0.94 * grid_x / columns,
                              0.94 * grid_y / rows);
        gl_Position = vec4(center.x + corner.x * half_size.x,
                           center.y + corner.y * half_size.y, 0.35, 1.0);
        face_color = vec3(1.0);
        pattern_data = vec4(uv, format_index,
                            params.time + float(instance) * 0.13);
        return;
    }

    if (scene_mode > 6.5) {
        vec2 corner = vertex == 0u ? vec2(-0.82, -0.68) :
                      vertex == 1u ? vec2( 0.82, -0.68) :
                      vertex == 2u ? vec2( 0.00,  0.88) :
                      vertex == 3u ? vec2(-0.82, -0.68) :
                      vertex == 4u ? vec2( 0.82, -0.68) :
                                     vec2( 0.00,  0.88);
        float columns = 32.0;
        float col = mod(float(instance), columns);
        float row = floor(float(instance) / columns);
        float rows = max(1.0, ceil(params.object_count / columns));
        float angle = params.time * 1.3 + float(instance) * 0.021;
        float ca = cos(angle);
        float sa = sin(angle);
        vec2 rotated = mat2(ca, -sa, sa, ca) * corner * 0.24;
        vec2 center = vec2(
            (col + 0.5) * 2.0 / columns - 1.0,
            (row + 0.5) * 2.0 / rows - 1.0
        );
        gl_Position = vec4((center.x + rotated.x) / max(params.aspect, 0.5),
                           center.y + rotated.y, 0.35, 1.0);
        float pulse = 0.72 + 0.28 * sin(params.time * 2.0 + float(instance) * 0.07);
        face_color = mix(vec3(0.08, 0.78, 0.96),
                         vec3(0.94, 0.25, 0.62),
                         mod(float(instance), 5.0) / 4.0) * pulse;
        apply_touch_rotation();
        return;
    }

    if (scene_mode > 5.5) {
        vec2 corner = vertex == 0u ? vec2(-1.0, -1.0) :
                      vertex == 1u ? vec2( 1.0, -1.0) :
                      vertex == 2u ? vec2( 1.0,  1.0) :
                      vertex == 3u ? vec2(-1.0, -1.0) :
                      vertex == 4u ? vec2( 1.0,  1.0) :
                                     vec2(-1.0,  1.0);
        float columns = 16.0;
        float col = mod(float(instance), columns);
        float row = floor(float(instance) / columns);
        float rows = max(1.0, ceil(params.object_count / columns));
        vec2 uv = corner * 0.5 + 0.5;
        float selector = mod(floor((uv.x + uv.y + float(instance) * 0.11 +
                                    params.time * 0.18) * 4.0), 4.0);
        vec3 bc_palette[4] = vec3[](
            vec3(0.05, 0.82, 0.96), vec3(0.95, 0.34, 0.22),
            vec3(0.30, 0.92, 0.48), vec3(0.98, 0.76, 0.20)
        );
        vec2 center = vec2(
            (col + 0.5) * 2.0 / columns - 1.0,
            1.0 - (row + 0.5) * 2.0 / rows
        );
        vec2 half_size = vec2(0.87 / columns, 0.80 / rows);
        gl_Position = vec4((center.x + corner.x * half_size.x) /
                               max(params.aspect, 0.5),
                           center.y + corner.y * half_size.y, 0.35, 1.0);
        float checker = mod(floor(uv.x * 4.0) + floor(uv.y * 4.0), 2.0);
        vec3 base = bc_palette[int(selector)];
        face_color = mix(base * 0.52, base, checker) *
                     (0.78 + 0.22 * sin(params.time * 1.8 + float(instance)));
        apply_touch_rotation();
        return;
    }

    if (scene_mode > 2.5) {
        vec3 control_points[3] = vec3[](
            vec3(-2.338, -1.60, 0.0),
            vec3( 2.338, -1.60, 0.0),
            vec3( 0.000,  2.45, 0.0)
        );
        gl_Position = vec4(control_points[vertex % 3u], 1.0);
        face_color = vertex % 3u == 0u ? vec3(0.16, 0.72, 0.96) :
                     vertex % 3u == 1u ? vec3(0.22, 0.94, 0.68) :
                                         vec3(0.95, 0.55, 0.20);
        return;
    }

    if (scene_mode > 1.5) {
        float angle = params.time * 1.6;
        vec2 point = vertex == 0u ? vec2(0.0, 0.72) :
                     vertex == 1u ? vec2(-0.62, -0.48) : vec2(0.62, -0.48);
        float ca = cos(angle);
        float sa = sin(angle);
        point = mat2(ca, -sa, sa, ca) * point;
        gl_Position = vec4(point.x / max(params.aspect, 0.5), point.y, 0.4, 1.0);
        face_color = vertex == 0u ? vec3(1.0, 0.36, 0.18) :
                     vertex == 1u ? vec3(0.18, 0.82, 0.96) : vec3(0.48, 0.95, 0.48);
        apply_touch_rotation();
        return;
    }

    if (scene_mode > 0.5) {
        vec3 p = dolphin_vertex(vertex);
        if (vertex >= 432u && vertex < 456u)
            p.y += sin(params.time * 5.0) * 0.13 * smoothstep(0.0, 0.43, abs(p.y));
        float swim = sin(params.time * 1.8 + float(instance) * 0.7);
        float scale = 0.54;
        p *= scale;
        p.y += swim * 0.16;
        p.x += (mod(float(instance), 3.0) - 1.0) * 1.05;
        p.y += (floor(float(instance) / 3.0) - floor((params.object_count - 1.0) / 6.0)) * 0.78;
        float yaw = sin(params.time * 0.6 + float(instance)) * 0.12;
        p.xz = mat2(cos(yaw), -sin(yaw), sin(yaw), cos(yaw)) * p.xz;
        float depth = 3.4 + float(instance / 6u) * 1.25 - p.z;
        gl_Position = vec4(p.x * 1.10 / max(params.aspect, 0.5),
                           p.y * 1.42, depth - 0.12, depth);
        face_color = vertex < 432u ? vec3(0.14, 0.64, 0.88) :
                     vertex < 456u ? vec3(0.12, 0.42, 0.72) :
                     vertex < 492u ? vec3(0.28, 0.79, 0.96) :
                     vec3(0.09, 0.34, 0.60);
        apply_touch_rotation();
        return;
    }

    float columns = 8.0;
    float rows = 4.0;
    float layer_size = columns * rows;
    float layer_count = max(1.0, ceil(params.object_count / layer_size));
    float layer = layer_count - 1.0 - floor(float(instance) / layer_size);
    float col = mod(float(instance), columns);
    float row = mod(floor(float(instance) / columns), rows);
    vec3 p = cube_vertices[vertex] * 0.27;
    float angle = params.time * 0.75 + float(instance) * 0.061;
    float ca = cos(angle);
    float sa = sin(angle);
    float cb = cos(angle * 0.71 + 0.4);
    float sb = sin(angle * 0.71 + 0.4);
    p = vec3(p.x * ca - p.z * sa, p.y, p.x * sa + p.z * ca);
    p = vec3(p.x, p.y * cb - p.z * sb, p.y * sb + p.z * cb);
    p.x += (col - 3.5) * 0.72;
    p.y += (row - 1.5) * 0.76;
    float depth = 4.8 + layer * 1.25 - p.z;
    gl_Position = vec4(p.x * 1.55 / max(params.aspect, 0.5),
                       p.y * 1.55, depth - 0.15, depth);
    face_color = palette[(vertex / 6u + instance) % 6u];
    apply_touch_rotation();
}
