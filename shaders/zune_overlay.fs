#version 100
#pragma profilepragma blendoperation(gl_FragColor, GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)
precision mediump float;
varying vec4 vColor;
void main() {
    gl_FragColor = vColor;
}
