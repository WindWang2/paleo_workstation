#version 330 core

in vec2 TexCoord;

uniform sampler2D sliceTexture;

out vec4 FragColor;

void main()
{
    FragColor = texture(sliceTexture, TexCoord);
}
