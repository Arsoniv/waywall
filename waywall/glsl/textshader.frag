#version 100

precision mediump float;

varying vec2 TexCoords;
uniform sampler2D textTexture;
uniform vec4 textColor;

void main() {
    float alpha = texture2D(textTexture, TexCoords).r;
    gl_FragColor = vec4(textColor.rgb, alpha * textColor.a);
}