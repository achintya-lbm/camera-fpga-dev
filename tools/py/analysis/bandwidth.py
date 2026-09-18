"""Link and stream bandwidth arithmetic for the HSB data plane (DESIGN.md §4)."""

from __future__ import annotations

# Ethernet 14 + IPv4 20 + UDP 8 + BTH 12 + RETH 16 + iCRC 4.
HSB_PACKET_OVERHEAD_BYTES = 74
# Preamble + SFD (8) and inter-frame gap (12) on the wire.
ETHERNET_LINE_OVERHEAD_BYTES = 20
HSB_PAGE_BYTES = 128


def payload_per_packet(mtu: int) -> int:
    """Sensor payload bytes per HSB packet at a given MTU (whole 128-byte pages)."""
    if mtu <= HSB_PACKET_OVERHEAD_BYTES:
        raise ValueError(f"mtu {mtu} too small")
    return ((mtu - HSB_PACKET_OVERHEAD_BYTES) // HSB_PAGE_BYTES) * HSB_PAGE_BYTES


def link_payload_gbps(link_gbps: float, mtu: int) -> float:
    """Usable sensor payload rate on a link after packet and line overhead."""
    payload = payload_per_packet(mtu)
    wire = payload + HSB_PACKET_OVERHEAD_BYTES + ETHERNET_LINE_OVERHEAD_BYTES
    return link_gbps * payload / wire


def stream_gbps(width: int, height: int, bits_per_pixel: int, fps: float) -> float:
    """Raw sensor payload rate for one stream."""
    return width * height * bits_per_pixel * fps / 1e9


def packets_per_second(width: int, height: int, bits_per_pixel: int, fps: float, mtu: int) -> float:
    """HSB packets per second one stream generates at a given MTU."""
    bytes_per_frame = width * height * bits_per_pixel / 8
    return fps * -(-bytes_per_frame // payload_per_packet(mtu))  # ceil division


def max_fps(width: int, height: int, bits_per_pixel: int, budget_gbps: float) -> float:
    """Highest frame rate whose payload fits the given budget."""
    return budget_gbps * 1e9 / (width * height * bits_per_pixel)
