/*
   Pixel perfect: every source pixel becomes an exact block of screen pixels (the engine only
   uses integer scales for this look), sampled with nearest filtering. No blending of any kind.
*/

#if defined(VERTEX)

#if __VERSION__ >= 130
#define COMPAT_VARYING out
#define COMPAT_ATTRIBUTE in
#else
#define COMPAT_VARYING varying
#define COMPAT_ATTRIBUTE attribute
#endif

COMPAT_ATTRIBUTE vec4 VertexCoord;
COMPAT_ATTRIBUTE vec4 TexCoord;
COMPAT_VARYING vec4 TEX0;
uniform mat4 MVPMatrix;

void main()
{
    gl_Position = MVPMatrix * VertexCoord;
    TEX0.xy = TexCoord.xy;
}

#elif defined(FRAGMENT)

#ifdef GL_ES
precision highp float;
#endif

#if __VERSION__ >= 130
#define COMPAT_VARYING in
#define COMPAT_TEXTURE texture
out vec4 FragColor;
#else
#define COMPAT_VARYING varying
#define FragColor gl_FragColor
#define COMPAT_TEXTURE texture2D
#endif

uniform vec2 TextureSize;
uniform sampler2D Texture;
COMPAT_VARYING vec4 TEX0;

void main()
{
    vec2 texel = floor(TEX0.xy * TextureSize) + 0.5;
    FragColor = vec4(COMPAT_TEXTURE(Texture, texel / TextureSize).rgb, 1.0);
}
#endif
