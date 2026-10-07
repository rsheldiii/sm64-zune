#version 100
// Touch overlay: positions arrive in rotated panel clip space (zune_overlay.h).
attribute vec2 aPos;
attribute vec4 aColor;
varying vec4 vColor;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vColor = aColor;
}
