"""
Generate doc/architecture-component.png — RemSvc + Airflow component diagram.
Run with:  python doc/_gen_arch_diagram.py
"""

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch
import matplotlib.patheffects as pe

# ── Palette ───────────────────────────────────────────────────────────────────
C_AIRFLOW  = "#e8f4f8"   # Airflow host background
C_REMOTE   = "#fff8ee"   # Remote host background
C_BOX_AF   = "#4a90d9"   # Airflow component border
C_BOX_REM  = "#d9904a"   # Remote component border
C_GRPC     = "#6aa84f"   # gRPC channel
C_SEC      = "#cc4444"   # Security layer
C_DB       = "#7b68ee"   # Metadata DB
C_STREAM   = "#2e86ab"   # Stream arrow
C_TEXT     = "#1a1a2e"
C_SUB      = "#555577"

FIG_W, FIG_H = 18, 11

fig, ax = plt.subplots(figsize=(FIG_W, FIG_H))
ax.set_xlim(0, FIG_W)
ax.set_ylim(0, FIG_H)
ax.axis("off")
fig.patch.set_facecolor("#f9f9f9")

# ── helpers ───────────────────────────────────────────────────────────────────

def box(ax, x, y, w, h, facecolor, edgecolor, lw=1.5, radius=0.25, alpha=1.0):
    p = FancyBboxPatch((x, y), w, h,
                       boxstyle=f"round,pad=0,rounding_size={radius}",
                       facecolor=facecolor, edgecolor=edgecolor,
                       linewidth=lw, alpha=alpha, zorder=3)
    ax.add_patch(p)
    return p

def label(ax, x, y, text, size=9, color=C_TEXT, bold=False, ha="center", va="center"):
    weight = "bold" if bold else "normal"
    ax.text(x, y, text, ha=ha, va=va, fontsize=size,
            color=color, fontweight=weight, zorder=5,
            fontfamily="monospace" if "`" in text else "sans-serif")

def section(ax, x, y, w, h, title, bg, border, title_color):
    """Labelled host background panel."""
    p = FancyBboxPatch((x, y), w, h,
                       boxstyle="round,pad=0,rounding_size=0.35",
                       facecolor=bg, edgecolor=border,
                       linewidth=2.2, zorder=1, alpha=0.55)
    ax.add_patch(p)
    ax.text(x + 0.25, y + h - 0.28, title,
            ha="left", va="top", fontsize=10, color=title_color,
            fontweight="bold", zorder=6)

def arrow(ax, x0, y0, x1, y1, color, lw=1.8, style="->", dashed=False):
    ls = (0, (5, 4)) if dashed else "solid"
    ax.annotate("", xy=(x1, y1), xytext=(x0, y0),
                arrowprops=dict(arrowstyle=style, color=color,
                                lw=lw, linestyle=ls),
                zorder=4)

def bidir(ax, x0, y0, x1, y1, color, lw=2.0):
    ax.annotate("", xy=(x1, y1), xytext=(x0, y0),
                arrowprops=dict(arrowstyle="<->", color=color,
                                lw=lw, connectionstyle="arc3,rad=0.0"),
                zorder=4)

# ── Host panels ───────────────────────────────────────────────────────────────
# Airflow host: x=0.4 .. 8.8,  y=0.5..10.5
section(ax, 0.4, 0.5, 8.4, 10.0, "Airflow Host", C_AIRFLOW, C_BOX_AF, C_BOX_AF)

# Remote host: x=10.2..17.3,  y=0.5..10.5
section(ax, 10.2, 0.5, 6.8, 10.0, "Remote Host", C_REMOTE, C_BOX_REM, C_BOX_REM)

# ── Airflow components ────────────────────────────────────────────────────────

# Scheduler
box(ax, 0.8, 8.5, 3.2, 1.4, "white", C_BOX_AF, lw=1.5)
label(ax, 2.4, 9.5, "Scheduler", size=10, bold=True, color=C_BOX_AF)
label(ax, 2.4, 9.1, "task orchestration", size=8, color=C_SUB)

# Worker pool
box(ax, 0.8, 6.5, 3.2, 1.5, "white", C_BOX_AF, lw=1.5)
label(ax, 2.4, 7.55, "Worker Pool", size=10, bold=True, color=C_BOX_AF)
label(ax, 2.4, 7.18, "execute()  /  execute_complete()", size=7.5, color=C_SUB)
label(ax, 2.4, 6.82, "slot held only briefly", size=7.5, color=C_SUB)

# Triggerer
box(ax, 0.8, 3.8, 3.2, 2.2, "white", C_BOX_AF, lw=2.0)
label(ax, 2.4, 5.55, "Triggerer Process", size=10, bold=True, color=C_BOX_AF)
label(ax, 2.4, 5.18, "asyncio event loop", size=8, color=C_SUB)
label(ax, 2.4, 4.82, "RemSvcTrigger", size=8.5, color=C_BOX_AF)
label(ax, 2.4, 4.46, "_write() / _read() concurrently", size=7.5, color=C_SUB)
label(ax, 2.4, 4.10, "retry  ·  backoff  ·  channel pool", size=7.5, color=C_SUB)

# Metadata DB
box(ax, 4.6, 6.0, 3.8, 3.5, "white", C_DB, lw=1.5, radius=0.3)
label(ax, 6.5, 9.0,  "Metadata DB", size=10, bold=True, color=C_DB)
label(ax, 6.5, 8.6,  "PostgreSQL / MySQL", size=8, color=C_SUB)
label(ax, 6.5, 8.15, "─────────────────────", size=7, color="#cccccc")
label(ax, 6.5, 7.75, "• Airflow Connections", size=8, color=C_SUB, ha="center")
label(ax, 6.5, 7.38, "  (host · port · TLS · bearer_token)", size=7.5, color=C_SUB)
label(ax, 6.5, 7.02, "• Trigger state (serialised)", size=8, color=C_SUB)
label(ax, 6.5, 6.65, "• XCom results", size=8, color=C_SUB)
label(ax, 6.5, 6.28, "• Task instance state", size=8, color=C_SUB)

# Airflow Provider package note
box(ax, 4.6, 3.8, 3.8, 1.8, "#f0f8ff", C_BOX_AF, lw=1.2, alpha=0.8)
label(ax, 6.5, 5.25, "airflow-provider-remsvc", size=9, bold=True, color=C_BOX_AF)
label(ax, 6.5, 4.88, "RemSvcOperator", size=8.5, color=C_SUB)
label(ax, 6.5, 4.55, "RemSvcTrigger", size=8.5, color=C_SUB)
label(ax, 6.5, 4.22, "RemSvcHook  ·  remsvc_proto/", size=8, color=C_SUB)

# ── Remote host components ────────────────────────────────────────────────────

# RemSvc_server
box(ax, 10.6, 7.2, 5.8, 2.8, "white", C_BOX_REM, lw=2.0)
label(ax, 13.5, 9.55, "RemSvc_server", size=11, bold=True, color=C_BOX_REM)
label(ax, 13.5, 9.12, "gRPC server  ·  port 50051", size=8.5, color=C_SUB)
label(ax, 13.5, 8.68, "─────────────────────────────", size=7, color="#cccccc")
label(ax, 13.5, 8.28, "BearerTokenAuthProcessor", size=8.5, color=C_SEC)
label(ax, 13.5, 7.92, "Command allowlist  (std::regex)", size=8.5, color=C_SUB)
label(ax, 13.5, 7.56, "RemSvcServiceImpl  ·  runInProcess()", size=8.5, color=C_SUB)

# Child processes
box(ax, 10.6, 4.5, 5.8, 2.2, "white", C_BOX_REM, lw=1.5)
label(ax, 13.5, 6.25, "Child Processes  (QProcess)", size=10, bold=True, color=C_BOX_REM)
label(ax, 13.5, 5.88, "cmd.exe /C   or   /bin/sh -c", size=8.5, color=C_SUB)
label(ax, 13.5, 5.52, "powershell.exe   or   pwsh", size=8.5, color=C_SUB)
label(ax, 13.5, 5.15, "one process per command  ·  isolated PID", size=8, color=C_SUB)
label(ax, 13.5, 4.78, "cmdTimeoutMs kill deadline", size=8, color=C_SEC)

# server.ini
box(ax, 10.6, 2.2, 2.6, 1.9, "white", "#6aa84f", lw=1.3)
label(ax, 11.9, 3.65, "server.ini", size=9, bold=True, color="#3d7a2a")
label(ax, 11.9, 3.28, "[server]  [tls]", size=8, color=C_SUB)
label(ax, 11.9, 2.95, "[auth]  [allowlist]", size=8, color=C_SUB)
label(ax, 11.9, 2.62, "[log]", size=8, color=C_SUB)

# Certs
box(ax, 13.8, 2.2, 2.8, 1.9, "white", "#6aa84f", lw=1.3)
label(ax, 15.2, 3.65, "Certificates", size=9, bold=True, color="#3d7a2a")
label(ax, 15.2, 3.28, "server.crt  /  server.key", size=7.8, color=C_SUB)
label(ax, 15.2, 2.95, "ca.pem  (mutual TLS)", size=7.8, color=C_SUB)
label(ax, 15.2, 2.62, "$VAR expansion in paths", size=7.5, color=C_SUB)

# ── Internal arrows: Airflow host ─────────────────────────────────────────────
# Scheduler ↔ Worker
arrow(ax, 2.4, 8.5, 2.4, 8.0, C_BOX_AF, lw=1.5)
arrow(ax, 2.6, 8.0, 2.6, 8.5, C_BOX_AF, lw=1.5)

# Scheduler ↔ Triggerer
arrow(ax, 2.0, 6.5, 2.0, 6.0, C_BOX_AF, lw=1.5)
arrow(ax, 2.4, 6.0, 2.4, 6.5, C_BOX_AF, lw=1.5)

# Worker ↔ Metadata DB
bidir(ax, 4.0, 7.3, 4.6, 7.8, C_DB, lw=1.5)

# Triggerer ↔ Metadata DB
bidir(ax, 4.0, 4.9, 4.6, 6.3, C_DB, lw=1.5)

# Scheduler ↔ Metadata DB
bidir(ax, 4.0, 9.2, 4.6, 8.5, C_DB, lw=1.5)

# Provider package → Triggerer (dashed, "used by")
arrow(ax, 6.5, 3.8, 2.4, 6.0, C_BOX_AF, lw=1.2, dashed=True)
label(ax, 4.55, 4.85, "uses", size=7.5, color=C_BOX_AF)

# ── Internal arrows: Remote host ──────────────────────────────────────────────
# server → child processes
arrow(ax, 13.5, 7.2, 13.5, 6.7, C_BOX_REM, lw=1.8)
label(ax, 14.15, 6.96, "spawn", size=7.5, color=C_BOX_REM)
arrow(ax, 13.2, 6.7, 13.2, 7.2, C_BOX_REM, lw=1.5)
label(ax, 12.5, 6.96, "rc + stdout\n+ stderr", size=7.5, color=C_BOX_REM, ha="center")

# server.ini → server
arrow(ax, 12.3, 4.1, 12.3, 7.2, "#6aa84f", lw=1.3, dashed=True)
label(ax, 11.62, 5.85, "loaded\nat startup", size=7.5, color="#3d7a2a", ha="center")

# certs → server
arrow(ax, 15.0, 4.1, 14.4, 7.2, "#6aa84f", lw=1.3, dashed=True)
label(ax, 15.35, 5.85, "read\nat startup", size=7.5, color="#3d7a2a", ha="center")

# ── gRPC channel ──────────────────────────────────────────────────────────────
# Main bidirectional stream: Triggerer ↔ RemSvc server
ax.annotate("", xy=(10.6, 8.4), xytext=(8.2, 4.9),
            arrowprops=dict(arrowstyle="<->", color=C_STREAM,
                            lw=2.8, connectionstyle="arc3,rad=-0.18"),
            zorder=4)

# Label on the channel
ax.text(9.55, 7.1, "RemCmdStrm\n(bidirectional stream)\nTLS optional\nAuthorization: Bearer …",
        ha="center", va="center", fontsize=8.5, color=C_STREAM,
        fontweight="bold", zorder=6,
        bbox=dict(facecolor="white", edgecolor=C_STREAM, lw=1.2,
                  boxstyle="round,pad=0.35", alpha=0.92))

# Worker → server (unary RPCs, future)
arrow(ax, 4.0, 7.15, 10.6, 8.2, C_BOX_AF, lw=1.3, dashed=True)
ax.text(7.55, 7.95, "Unary RPCs (Ping, GetStatus)",
        ha="center", va="center", fontsize=7.5, color=C_BOX_AF,
        style="italic", zorder=6)

# ── Title & legend ────────────────────────────────────────────────────────────
ax.text(FIG_W / 2, 10.72,
        "RemSvc — Architecture & Component Diagram",
        ha="center", va="center", fontsize=14, fontweight="bold",
        color=C_TEXT, zorder=7)

legend_items = [
    mpatches.Patch(facecolor=C_AIRFLOW, edgecolor=C_BOX_AF, lw=1.5, label="Airflow Host"),
    mpatches.Patch(facecolor=C_REMOTE,  edgecolor=C_BOX_REM, lw=1.5, label="Remote Host"),
    mpatches.Patch(facecolor="white",   edgecolor=C_STREAM,  lw=1.5, label="gRPC channel (RemCmdStrm)"),
    mpatches.Patch(facecolor="white",   edgecolor=C_SEC,     lw=1.5, label="Security enforcement"),
    mpatches.Patch(facecolor="white",   edgecolor="#6aa84f", lw=1.5, label="Config / certificates"),
]
ax.legend(handles=legend_items, loc="lower left",
          bbox_to_anchor=(0.02, 0.01),
          fontsize=8, framealpha=0.9, edgecolor="#cccccc")

plt.tight_layout(pad=0.3)
out = "doc/architecture-component.png"
plt.savefig(out, dpi=160, bbox_inches="tight", facecolor=fig.get_facecolor())
print(f"Saved: {out}")
