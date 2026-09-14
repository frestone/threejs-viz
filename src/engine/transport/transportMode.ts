export type TransportMode = "ws" | "ffi";

export function resolveTransportMode(
  desktop: boolean,
  explicit?: TransportMode,
): TransportMode {
  return explicit ?? (desktop ? "ffi" : "ws");
}