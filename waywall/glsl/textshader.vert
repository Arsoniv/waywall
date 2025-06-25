#version 100

attribute vec4 vertex;
varying vec2 TexCoords;
uniform vec2 u_dst_size;

void main() {
    vec2 position = vertex.xy; //

    vec2 ndc; //normalised position (from -1 to 1)

    //convert the pixel position to the normalised position
    ndc.x = (position.x / u_dst_size.x) * 2.0 - 1.0;
    ndc.y = (position.y / u_dst_size.y) * 2.0 - 1.0;

    gl_Position = vec4(ndc, 0.0, 1.0);

    TexCoords = vertex.zw;
}