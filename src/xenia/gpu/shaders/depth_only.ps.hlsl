// Depth-only pixel shader for DXIL mode.
// This is a minimal shader that outputs nothing, allowing depth to be written.
// Used when there's no pixel shader but depth buffer operations are needed.

void main() {
  // No output - just allow depth testing/writing to occur.
  // The depth value is taken from the vertex shader's SV_Position.z
}
