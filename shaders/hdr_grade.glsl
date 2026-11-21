
vec3 OklabOf(vec3 c) {
    float l = 0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b;
    float m = 0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b;
    float s = 0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b;
    vec3 q = sign(vec3(l, m, s)) * pow(abs(vec3(l, m, s)), vec3(1.0 / 3.0));
    return vec3(0.2104542553 * q.x + 0.7936177850 * q.y - 0.0040720468 * q.z,
                1.9779984951 * q.x - 2.4285922050 * q.y + 0.4505937099 * q.z,
                0.0259040371 * q.x + 0.7827717662 * q.y - 0.8086757660 * q.z);
}

vec3 LinearOfOklab(vec3 lab) {
    float l = lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z;
    float m = lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z;
    float s = lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z;
    l = l * l * l;
    m = m * m * m;
    s = s * s * s;
    return vec3(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
                -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s);
}

vec3 HdrGrade(vec3 light, vec4 look0, vec4 look1, float peak) {
    const float kMid = 0.18;
    light *= look0.x;
    float y = dot(light, vec3(0.2126, 0.7152, 0.0722));
    if (y > 1.0e-7) {
        float over = y / kMid;
        float high = y / (y + kMid);
        float power = look0.y - 1.0 + (look0.z - 1.0) * high * high + 0.5 * (1.0 - look0.w) * (1.0 - high) * (1.0 - high);
        light *= pow(over, power);
        y = dot(light, vec3(0.2126, 0.7152, 0.0722));
    }
    float chroma = look1.x * mix(1.0, look1.y, smoothstep(kMid, 1.0, y)) * (1.0 - look1.z * smoothstep(1.0, max(peak, 1.01), y));
    if (abs(chroma - 1.0) > 1.0e-4) {
        vec3 lab = OklabOf(light);
        lab.yz *= chroma;
        light = LinearOfOklab(lab);
    }
    return light;
}

vec3 GamutCompress(vec3 c, vec3 luma) {
    const float kKnee = 0.95;
    float y = dot(c, luma);
    if (y <= 1.0e-7) {
        return max(c, vec3(0.0));
    }
    float reach = max((y - c.r) / y, max((y - c.g) / y, (y - c.b) / y));
    if (reach <= kKnee) {
        return c;
    }
    float kept = kKnee + (1.0 - kKnee) * (1.0 - exp(-(reach - kKnee) / (1.0 - kKnee)));
    return y + (c - y) * (kept / reach);
}

vec3 HdrGamut(vec3 light, float mode) {
    if (mode < 0.5) {
        return GamutCompress(light, vec3(0.2126, 0.7152, 0.0722));
    }
    const mat3 k709ToP3 = mat3(0.8224621, 0.0331941, 0.0170827,
                               0.1775380, 0.9668058, 0.0723974,
                               0.0, 0.0, 0.9105199);
    const mat3 kP3To709 = mat3(1.2249401, -0.0420569, -0.0196376,
                               -0.2249404, 1.0420571, -0.0786361,
                               0.0, 0.0, 1.0982735);
    return kP3To709 * GamutCompress(k709ToP3 * light, vec3(0.2289746, 0.6917385, 0.0792869));
}
