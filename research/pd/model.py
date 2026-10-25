"""Causal three-block PD ModernTCN; core definitions retained from the upstream model."""
from __future__ import annotations

from dataclasses import dataclass
import torch
from torch import nn


@dataclass(frozen=True)
class V9EdgeModernTCNConfig:
    """Configuration for the causal pointwise V9 onboard Student.

    ``layers`` is deliberately explicit because the flight candidate is
    constrained to three ModernTCN blocks.  The model emits one risk logit per
    input time step; a streaming application consumes the final logit while
    offline evaluation can retain the complete event trajectory.
    """

    input_channels: int
    d_model: int = 64
    layers: int = 3
    kernel_sizes: tuple[int, ...] = (7, 15, 31)
    small_kernel_size: int = 3
    expansion_ratio: int = 4
    dropout: float = 0.05
    architecture: str = "flat"
    stream_count: int = 12
    features_per_stream: int = 8
    channels_per_fov: int = 3
    stream_d_model: int = 64
    fov_d_model: int = 128
    direct_input_residual: bool = False
    direct_input_residual_scale: float = 1.0


class _CausalModernTCNBlock(nn.Module):
    """Causal same-resolution ModernTCN block.

    Large/small depthwise branches perform temporal token mixing and the
    inverted pointwise FFN performs channel mixing. Left-only padding keeps
    every output causal.
    """

    def __init__(
        self,
        channels: int,
        *,
        large_kernel_size: int,
        small_kernel_size: int,
        expansion_ratio: int,
        dropout: float,
    ) -> None:
        super().__init__()
        large_kernel = int(large_kernel_size)
        small_kernel = min(int(small_kernel_size), large_kernel)
        if large_kernel < 3 or large_kernel % 2 == 0:
            raise ValueError("large_kernel_size must be an odd integer >= 3")
        if small_kernel < 1 or small_kernel % 2 == 0:
            raise ValueError("small_kernel_size must be a positive odd integer")
        hidden = int(channels) * max(2, int(expansion_ratio))
        self.large_left_padding = large_kernel - 1
        self.small_left_padding = small_kernel - 1
        self.token_norm = nn.LayerNorm(int(channels))
        self.large_depthwise = nn.Conv1d(
            int(channels),
            int(channels),
            kernel_size=large_kernel,
            groups=int(channels),
        )
        self.small_depthwise = nn.Conv1d(
            int(channels),
            int(channels),
            kernel_size=small_kernel,
            groups=int(channels),
        )
        self.ffn_norm = nn.LayerNorm(int(channels))
        self.ffn = nn.Sequential(
            nn.Conv1d(int(channels), hidden, kernel_size=1),
            nn.GELU(),
            nn.Dropout(float(dropout)),
            nn.Conv1d(hidden, int(channels), kernel_size=1),
        )
        self.dropout = nn.Dropout(float(dropout))
        self.token_scale = nn.Parameter(torch.full((int(channels),), 1.0e-2))
        self.ffn_scale = nn.Parameter(torch.full((int(channels),), 1.0e-2))

    @staticmethod
    def _channel_last_norm(norm: nn.LayerNorm, values: torch.Tensor) -> torch.Tensor:
        return norm(values.transpose(1, 2)).transpose(1, 2)

    def forward(self, inputs: torch.Tensor) -> torch.Tensor:
        hidden = self._channel_last_norm(self.token_norm, inputs)
        large = self.large_depthwise(
            nn.functional.pad(hidden, (self.large_left_padding, 0))
        )
        small = self.small_depthwise(
            nn.functional.pad(hidden, (self.small_left_padding, 0))
        )
        inputs = inputs + self.dropout(large + small) * self.token_scale.view(
            1, -1, 1
        )
        hidden = self._channel_last_norm(self.ffn_norm, inputs)
        return inputs + self.dropout(self.ffn(hidden)) * self.ffn_scale.view(
            1, -1, 1
        )


class V9EdgeModernTCNStudent(nn.Module):
    """Three-layer causal ModernTCN distilled from the released V9 Teacher.

    Input features are causal functions of five-minute count-rate and validity
    masks.  The output has shape ``(B, T)`` so event onset, duration, and decay
    remain observable instead of being collapsed into one window score.
    """

    def __init__(self, config: V9EdgeModernTCNConfig) -> None:
        super().__init__()
        if int(config.layers) != 3:
            raise ValueError("the V9 onboard ModernTCN Student must use exactly 3 layers")
        kernels = tuple(int(value) for value in config.kernel_sizes)
        if len(kernels) != int(config.layers):
            raise ValueError("kernel_sizes must contain one value per ModernTCN layer")
        self.config = config
        architecture = str(config.architecture)
        if architecture not in {"flat", "stream_fov_hierarchy"}:
            raise ValueError(
                "architecture must be one of: flat, stream_fov_hierarchy"
            )
        self.input_projection: nn.Module | None = None
        self.stream_projection: nn.Module | None = None
        self.stream_embedding: nn.Parameter | None = None
        self.fov_projection: nn.Module | None = None
        self.fov_embedding: nn.Parameter | None = None
        self.global_projection: nn.Module | None = None
        self.direct_risk_head: nn.Module | None = None
        if architecture == "flat":
            self.input_projection = nn.Conv1d(
                int(config.input_channels),
                int(config.d_model),
                kernel_size=1,
            )
            self.blocks = nn.Sequential(
                *[
                    _CausalModernTCNBlock(
                        int(config.d_model),
                        large_kernel_size=kernel,
                        small_kernel_size=int(config.small_kernel_size),
                        expansion_ratio=int(config.expansion_ratio),
                        dropout=float(config.dropout),
                    )
                    for kernel in kernels
                ]
            )
        else:
            streams = int(config.stream_count)
            per_stream = int(config.features_per_stream)
            channels_per_fov = int(config.channels_per_fov)
            if int(config.input_channels) != streams * per_stream:
                raise ValueError(
                    "hierarchical input_channels must equal "
                    "stream_count * features_per_stream"
                )
            if streams % channels_per_fov != 0:
                raise ValueError("stream_count must be divisible by channels_per_fov")
            fov_count = streams // channels_per_fov
            stream_dim = int(config.stream_d_model)
            fov_dim = int(config.fov_d_model)
            self.stream_projection = nn.Linear(per_stream, stream_dim)
            self.stream_embedding = nn.Parameter(torch.zeros(streams, stream_dim))
            nn.init.normal_(self.stream_embedding, mean=0.0, std=0.02)
            self.fov_projection = nn.Sequential(
                nn.Linear(channels_per_fov * stream_dim, fov_dim),
                nn.GELU(),
                nn.Dropout(float(config.dropout)),
            )
            self.fov_embedding = nn.Parameter(torch.zeros(fov_count, fov_dim))
            nn.init.normal_(self.fov_embedding, mean=0.0, std=0.02)
            self.global_projection = nn.Sequential(
                nn.Linear(fov_count * fov_dim, int(config.d_model)),
                nn.GELU(),
                nn.Dropout(float(config.dropout)),
            )
            self.blocks = nn.ModuleList(
                [
                    _CausalModernTCNBlock(
                        stream_dim,
                        large_kernel_size=kernels[0],
                        small_kernel_size=int(config.small_kernel_size),
                        expansion_ratio=int(config.expansion_ratio),
                        dropout=float(config.dropout),
                    ),
                    _CausalModernTCNBlock(
                        fov_dim,
                        large_kernel_size=kernels[1],
                        small_kernel_size=int(config.small_kernel_size),
                        expansion_ratio=int(config.expansion_ratio),
                        dropout=float(config.dropout),
                    ),
                    _CausalModernTCNBlock(
                        int(config.d_model),
                        large_kernel_size=kernels[2],
                        small_kernel_size=int(config.small_kernel_size),
                        expansion_ratio=int(config.expansion_ratio),
                        dropout=float(config.dropout),
                    ),
                ]
            )
        if bool(config.direct_input_residual):
            self.direct_risk_head = nn.Conv1d(
                int(config.input_channels),
                1,
                kernel_size=1,
            )
            nn.init.zeros_(self.direct_risk_head.weight)
            nn.init.zeros_(self.direct_risk_head.bias)
        hidden = max(16, int(config.d_model) // 2)
        self.output_norm = nn.LayerNorm(int(config.d_model))
        self.risk_head = nn.Sequential(
            nn.Conv1d(int(config.d_model), hidden, kernel_size=1),
            nn.GELU(),
            nn.Dropout(float(config.dropout)),
            nn.Conv1d(hidden, 1, kernel_size=1),
        )

    @property
    def parameter_count(self) -> int:
        return int(sum(parameter.numel() for parameter in self.parameters()))

    def forward(self, features: torch.Tensor) -> torch.Tensor:
        if features.ndim != 3:
            raise ValueError("features must have shape (B,T,C)")
        if features.shape[-1] != int(self.config.input_channels):
            raise ValueError(f"expected {self.config.input_channels} input channels")
        if str(self.config.architecture) == "flat":
            assert self.input_projection is not None
            hidden = self.input_projection(features.transpose(1, 2).contiguous())
            hidden = self.blocks(hidden)
        else:
            assert self.stream_projection is not None
            assert self.stream_embedding is not None
            assert self.fov_projection is not None
            assert self.fov_embedding is not None
            assert self.global_projection is not None
            batch, steps, _ = features.shape
            streams = int(self.config.stream_count)
            per_stream = int(self.config.features_per_stream)
            channels_per_fov = int(self.config.channels_per_fov)
            fov_count = streams // channels_per_fov
            stream_dim = int(self.config.stream_d_model)
            fov_dim = int(self.config.fov_d_model)

            stream = features.reshape(batch, steps, streams, per_stream)
            stream = self.stream_projection(stream)
            stream = stream + self.stream_embedding.view(1, 1, streams, stream_dim)
            stream = stream.permute(0, 2, 3, 1).reshape(
                batch * streams, stream_dim, steps
            )
            stream = self.blocks[0](stream)
            stream = stream.reshape(batch, streams, stream_dim, steps).permute(
                0, 3, 1, 2
            )

            fov = stream.reshape(
                batch,
                steps,
                fov_count,
                channels_per_fov * stream_dim,
            )
            fov = self.fov_projection(fov)
            fov = fov + self.fov_embedding.view(1, 1, fov_count, fov_dim)
            fov = fov.permute(0, 2, 3, 1).reshape(
                batch * fov_count, fov_dim, steps
            )
            fov = self.blocks[1](fov)
            fov = fov.reshape(batch, fov_count, fov_dim, steps).permute(
                0, 3, 1, 2
            )

            hidden = self.global_projection(
                fov.reshape(batch, steps, fov_count * fov_dim)
            ).transpose(1, 2)
            hidden = self.blocks[2](hidden)
        hidden = self.output_norm(hidden.transpose(1, 2)).transpose(1, 2)
        logits = self.risk_head(hidden).squeeze(1)
        if self.direct_risk_head is not None:
            direct = self.direct_risk_head(
                features.transpose(1, 2).contiguous()
            ).squeeze(1)
            logits = logits + float(self.config.direct_input_residual_scale) * direct
        return logits
