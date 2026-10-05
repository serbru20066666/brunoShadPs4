// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    bool srgb_input;
    float enhance;
    vec2 texel;
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

float luma(vec3 rgb) {
    return dot(rgb, vec3(0.2126, 0.7152, 0.0722));
}

vec3 load(vec2 at) {
    vec3 rgb = texture(texSampler, at).rgb;
    return pp.srgb_input ? degamma(rgb) : rgb;
}

// "Enhance Game Quality": brings out detail the image already has. Works on linear colour.
vec3 enhance_detail(vec3 rgb, vec2 at, float strength) {
    // Eight directions, at a tight and at a wide radius.
    const vec2 ring[8] = vec2[](vec2(1.0, 0.0), vec2(0.707, 0.707), vec2(0.0, 1.0),
                                vec2(-0.707, 0.707), vec2(-1.0, 0.0), vec2(-0.707, -0.707),
                                vec2(0.0, -1.0), vec2(0.707, -0.707));
    float fine = 0.0;
    float wide = 0.0;
    for (int i = 0; i < 8; ++i) {
        fine += luma(load(at + ring[i] * pp.texel * 1.5));
        wide += luma(load(at + ring[i] * pp.texel * 9.0)) +
                luma(load(at + ring[i].yx * vec2(1.0, -1.0) * pp.texel * 18.0));
    }
    fine /= 8.0;
    wide /= 16.0;

    const float l = luma(rgb);
    // Local contrast against the surroundings, as a ratio so that dark areas are not crushed,
    // and less of it near black and white where it would clip.
    const float midtones = 1.0 - pow(abs(2.0 * sqrt(clamp(l, 0.0, 1.0)) - 1.0), 3.0);
    float gain = 1.0;
    gain += strength * 0.55 * midtones * clamp((l - wide) / max(wide, 0.04), -0.5, 0.5);
    // Fine sharpening on top.
    gain += strength * 0.60 * clamp((l - fine) / max(fine, 0.04), -0.35, 0.35);
    return max(rgb * gain, 0.0);
}

// The tonal part, on display encoded colour: a gentle contrast curve and more colour where
// there is little of it, leaving already saturated colours alone.
vec3 enhance_tone(vec3 rgb, float strength) {
    rgb = clamp(rgb, 0.0, 1.0);
    rgb = mix(rgb, rgb * rgb * (3.0 - 2.0 * rgb), 0.22 * strength);
    const float saturation = max(rgb.r, max(rgb.g, rgb.b)) - min(rgb.r, min(rgb.g, rgb.b));
    return mix(vec3(luma(rgb)), rgb, 1.0 + 0.28 * strength * (1.0 - saturation));
}

void main() {
    // A negative strength is the comparison view: the middle of the frame twice, as it is on
    // the left and enhanced on the right.
    vec2 at = uv;
    float strength = pp.enhance;
    if (pp.enhance < 0.0) {
        const bool right = uv.x >= 0.5;
        at.x = uv.x + (right ? -0.25 : 0.25);
        strength = right ? -pp.enhance : 0.0;
    }

    vec4 color_linear = texture(texSampler, at);
    if (pp.hdr) {
        color = color_linear;
    } else {
        if (pp.srgb_input) color_linear.rgb = degamma(color_linear.rgb);
        if (strength > 0.0) {
            color = vec4(enhance_tone(gamma(enhance_detail(color_linear.rgb, at, strength)),
                                      strength),
                         color_linear.a);
        } else {
            color = vec4(gamma(color_linear.rgb), color_linear.a);
        }
        if (pp.enhance < 0.0 && abs(uv.x - 0.5) < pp.texel.x) {
            color.rgb = vec3(1.0);
        }
    }
}
