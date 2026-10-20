#version 450

layout(push_constant) uniform Push {
    vec4 SourceSize;
    vec4 OriginalSize;
    vec4 OutputSize;
    uint FrameCount;
    float dummy1;
    float dummy2;
} params;

layout(std140, set = 0, binding = 0) uniform UBO {
    mat4 MVP;
} global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main() {
    gl_Position = global.MVP * Position;
    vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

float rand(vec2 co) {
    return fract(sin(dot(co.xy, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
    vec2 fragCoord = vTexCoord * params.OutputSize.xy;

    float screenAspect = params.OutputSize.x / params.OutputSize.y;
    bool isWide = screenAspect > 1.0;

    float sideThickness   = isWide ? 30.0 : 20.0;
    float topThickness    = isWide ? 30.0 : 20.0;
    float bottomThickness = topThickness;

    float cornerRadius = 40.0;
    float bezelCornerRadius = 80.0;
    float margin = 8.0;

    float screenSpacing = 0.0; // No divider line
    float dualScreenAspect = isWide
        ? (4.0 / 3.0) * 2.0
        : (4.0 / 3.0) / 2.0;

    float contentWidth;
    float contentHeight;

    if (screenAspect >= dualScreenAspect) {
        contentHeight = params.OutputSize.y - (topThickness + bottomThickness + 2.0 * margin);
        contentWidth = contentHeight * dualScreenAspect;
    } else {
        contentWidth = params.OutputSize.x - (2.0 * sideThickness + 2.0 * margin);
        contentHeight = contentWidth / dualScreenAspect;
    }

    float topMargin = isWide
        ? (params.OutputSize.y - contentHeight) * 0.5
        : params.OutputSize.y * 0.02;

    vec2 anchorTopCenter = vec2(params.OutputSize.x * 0.5, topMargin + contentHeight);
    vec2 minContent = anchorTopCenter - vec2(contentWidth * 0.5, contentHeight);
    vec2 maxContent = anchorTopCenter + vec2(contentWidth * 0.5, 0.0);

    vec2 minFrame = vec2(minContent.x - sideThickness, minContent.y - topThickness);
    vec2 maxFrame = vec2(maxContent.x + sideThickness, maxContent.y + bottomThickness);

    bool inContent = fragCoord.x >= minContent.x && fragCoord.x <= maxContent.x &&
                     fragCoord.y >= minContent.y && fragCoord.y <= maxContent.y;

    bool inFrame = fragCoord.x >= minFrame.x && fragCoord.x <= maxFrame.x &&
                   fragCoord.y >= minFrame.y && fragCoord.y <= maxFrame.y;

    bool roundedOut = false;
    vec2 frameCornerDist = min(fragCoord - minFrame, maxFrame - fragCoord);
    if (frameCornerDist.x < cornerRadius && frameCornerDist.y < cornerRadius) {
        float dx = cornerRadius - frameCornerDist.x;
        float dy = cornerRadius - frameCornerDist.y;
        if (dx * dx + dy * dy > cornerRadius * cornerRadius) {
            roundedOut = true;
        }
    }

    vec2 screenMin = vec2(0.0);
    vec2 screenMax = params.OutputSize.xy;
    vec2 bezelCornerDist = min(fragCoord - screenMin, screenMax - fragCoord);
    if (bezelCornerDist.x < bezelCornerRadius && bezelCornerDist.y < bezelCornerRadius) {
        float dx = bezelCornerRadius - bezelCornerDist.x;
        float dy = bezelCornerRadius - bezelCornerDist.y;
        if (dx * dx + dy * dy > bezelCornerRadius * bezelCornerRadius) {
            discard;
        }
    }

    float shadowIntensity = 0.0;
    float distLeft = fragCoord.x;
    float distRight = params.OutputSize.x - fragCoord.x;
    float distTop = fragCoord.y;
    float distBottom = params.OutputSize.y - fragCoord.y;
    float outerShadowWidth = 48.0;

    shadowIntensity += smoothstep(outerShadowWidth, 0.0, distLeft);
    shadowIntensity += smoothstep(outerShadowWidth, 0.0, distRight);
    shadowIntensity += smoothstep(outerShadowWidth, 0.0, distTop);
    shadowIntensity += smoothstep(outerShadowWidth, 0.0, distBottom);
    shadowIntensity = clamp(shadowIntensity * 0.25, 0.0, 0.25);

    float grain = (rand(fragCoord / 4.0) - 0.5) * 0.015;

    if (inContent) {
        vec2 scaledUV;

        if (isWide) {
            float screenWidth = (maxContent.x - minContent.x) * 0.5;
            bool isLeftScreen = fragCoord.x < (minContent.x + screenWidth);
            float xOffset = isLeftScreen ? 0.0 : screenWidth;
            vec2 screenOrigin = vec2(minContent.x + xOffset, minContent.y);
            vec2 screenSize = vec2(screenWidth, maxContent.y - minContent.y);
            scaledUV = (fragCoord - screenOrigin) / screenSize;
            scaledUV.x *= 0.5;
            if (!isLeftScreen) scaledUV.x += 0.5;

        } else {
            float screenHeight = (maxContent.y - minContent.y) * 0.5;
            bool isTopScreen = fragCoord.y < (minContent.y + screenHeight);
            float yOffset = isTopScreen ? 0.0 : screenHeight;
            vec2 screenOrigin = vec2(minContent.x, minContent.y + yOffset);
            vec2 screenSize = vec2(maxContent.x - minContent.x, screenHeight);
            scaledUV = (fragCoord - screenOrigin) / screenSize;
            scaledUV.y *= 0.5;
            if (!isTopScreen) scaledUV.y += 0.5;
        }

        FragColor = texture(Source, scaledUV);
    }
    else if (inFrame && !roundedOut) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
    }
    else {
        vec3 baseColor = vec3(0.961, 0.961, 0.961);
        baseColor += grain;
        baseColor *= 1.0 - shadowIntensity;
        FragColor = vec4(baseColor, 1.0);
    }
}