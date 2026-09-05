# The simulator: a SimNode speaks byte-identical ENLP (same protocol
# package, same Frame/message classes) through an in-process transport, and
# SimNetwork composes many of them into an ESP-NOW-like mesh with
# configurable loss/latency. This is the reason the desktop can be fully
# developed with zero hardware -- see docs/ARCHITECTURE.md's "Injectable
# transports" section.
from .node import SimNode, SimTransport
from .network import SimNetwork

__all__ = ["SimNode", "SimTransport", "SimNetwork"]
