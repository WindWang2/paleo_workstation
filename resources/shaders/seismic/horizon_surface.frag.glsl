#version 330 core

in vec3 FragPos;
in vec3 Normal;
in vec3 VertexColor;

uniform vec3 lightPos;
uniform vec3 viewPos;
uniform vec3 layerColor;
uniform int displayColorMode;
uniform float surfaceOpacity;

out vec4 FragColor;

void main()
{
    vec3 baseColor = VertexColor;
    if(displayColorMode == 1) {
        baseColor = layerColor;
    } else if(displayColorMode == 2) {
        float gray = dot(VertexColor, vec3(0.299, 0.587, 0.114));
        baseColor = vec3(gray);
    }

    vec3 normal = normalize(Normal);
    vec3 lightDir = normalize(lightPos - FragPos);
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 ambient = 0.28 * baseColor;
    vec3 diffuse = 0.72 * diff * baseColor;
    FragColor = vec4(ambient + diffuse, surfaceOpacity);
}
