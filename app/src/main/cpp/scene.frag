#version 450

layout(location = 0) in vec3 face_color;
layout(location = 1) in vec4 pattern_data;
layout(location = 0) out vec4 out_color;

void main() {
    if (pattern_data.z > 12.5 && pattern_data.z < 13.5) {
        float alpha = 0.42 * (1.0 - smoothstep(0.72, 1.0, pattern_data.w));
        if (alpha < 0.005)
            discard;
        out_color = vec4(face_color, alpha);
        return;
    }
    if (pattern_data.z > 11.5 && pattern_data.z < 12.5) {
        out_color = vec4(face_color, 1.0);
        return;
    }
    if (pattern_data.z > 13.5 && pattern_data.z < 14.5) {
        vec2 p = pattern_data.xy * 2.0 - 1.0;
        float wave = sin(length(p) * 22.0 - pattern_data.w * 3.0) *
                     cos(atan(p.y, p.x) * 8.0 + pattern_data.w);
        if (wave < -0.22)
            discard;
        float band = 0.5 + 0.5 * sin(pattern_data.w * 2.0 + p.x * 9.0);
        out_color = vec4(mix(face_color * 0.45, face_color, band), 1.0);
        return;
    }
    if (pattern_data.z > 14.5 && pattern_data.z < 15.5) {
        float pulse = 0.5 + 0.5 * sin(pattern_data.w + pattern_data.xy.y * 9.0);
        vec3 color = mix(face_color * 0.42, face_color, pulse);
        float alpha = 0.12 + 0.22 * pulse;
        out_color = vec4(color, alpha);
        return;
    }
    if (pattern_data.z > 10.5) {
        vec2 p = pattern_data.xy * 2.0 - 1.0;
        float field = 0.0;
        for (int i = 0; i < 20; i++) {
            float fi = float(i);
            p = abs(p) / max(dot(p, p), 0.08) -
                vec2(0.71 + 0.025 * sin(pattern_data.w + fi),
                     0.64 + 0.02 * cos(pattern_data.w * 0.7 + fi));
            field += sin(length(p) * (1.5 + fi * 0.12) + pattern_data.w);
        }
        vec3 color = 0.5 + 0.5 * cos(vec3(0.0, 2.1, 4.2) + field * 0.075);
        out_color = vec4(color, 1.0);
        return;
    }
    if (pattern_data.z > 9.5) {
        out_color = vec4(face_color, pattern_data.w);
        return;
    }
    if (pattern_data.z >= 0.0) {
        const vec3 format_colors[10] = vec3[](
            vec3(0.05, 0.78, 0.96), vec3(0.98, 0.34, 0.22),
            vec3(0.28, 0.90, 0.42), vec3(0.98, 0.76, 0.18),
            vec3(0.70, 0.34, 0.98), vec3(0.12, 0.86, 0.72),
            vec3(0.96, 0.28, 0.62), vec3(0.46, 0.66, 1.0),
            vec3(0.92, 0.58, 0.18), vec3(0.72, 0.88, 0.30)
        );
        int format_index = clamp(int(pattern_data.z), 0, 9);
        vec2 texel = floor(pattern_data.xy * 4.0);
        float selector = mod(texel.x + texel.y * 3.0 +
                             floor(pattern_data.w * 2.0), 4.0);
        vec3 endpoint0 = format_colors[format_index] * 0.34;
        vec3 endpoint1 = mix(format_colors[format_index],
                             vec3(1.0), 0.24);
        vec3 decoded_color = selector < 0.5 ? endpoint0 :
                             selector < 1.5 ? endpoint1 :
                             selector < 2.5 ? mix(endpoint0, endpoint1, 0.34) :
                                              mix(endpoint0, endpoint1, 0.67);
        float border = min(min(pattern_data.x, 1.0 - pattern_data.x),
                           min(pattern_data.y, 1.0 - pattern_data.y));
        out_color = vec4(border < 0.025 ? vec3(0.025, 0.045, 0.075) :
                         decoded_color, 1.0);
        return;
    }
    out_color = vec4(face_color, 1.0);
}
