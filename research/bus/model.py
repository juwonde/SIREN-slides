from __future__ import annotations

from dataclasses import dataclass

import torch
from torch import nn


@dataclass(frozen=True)
class AnonymousModernTCNConfig:
    feature_dim: int = 3
    hidden_channels: int = 24
    channel_blocks: int = 1
    relation_blocks: int = 0
    temporal_blocks: int = 4
    short_temporal_blocks: int = 0
    large_kernel: int = 15
    small_kernel: int = 3
    short_kernel: int = 5
    ffn_ratio: int = 2
    dropout: float = 0.1
    max_groups: int = 16
    all_member_pooling: bool = False
    causal_convolutions: bool = False
    dual_view_stem: bool = False
    temporal_dilation_cycle: int = 1
    category_head_count: int = 0
    category_union_output: bool = True
    recovery_head: bool = False
    sustained_head: bool = False
    sustained_direct_input: bool = False
    group_union_output: bool = False
    window_tail_pool_steps: int = 0
    dual_view_contrast: bool = False
    long_context_features: int = 0
    prewindow_level_contrast: bool = False
    sustained_prewindow_level_contrast: bool = False
    member_max_clip_sigma: float = 0.0
    sustained_member_max_clip_sigma: float = 0.0
    group_max_clip_sigma: float = 0.0
    modality_adapter_count: int = 0
    modality_adapter_rank: int = 16
    temporal_modality_adapters: bool = False
    modality_logit_calibration: bool = False
    member_head: bool = False
    member_temporal_blocks: int = 0
    member_temporal_residual: bool = False
    member_peer_context: bool = False


@dataclass(frozen=True)
class AnonymousModernTCNOutput:
    window_logits: torch.Tensor
    sequence_logits: torch.Tensor
    group_window_logits: torch.Tensor
    group_sequence_logits: torch.Tensor
    category_sequence_logits: torch.Tensor | None = None
    recovery_sequence_logits: torch.Tensor | None = None
    sustained_window_logits: torch.Tensor | None = None
    window_embedding: torch.Tensor | None = None
    member_sequence_logits: torch.Tensor | None = None


class ModernTCNBlock(nn.Module):
    """ModernTCN-style large/small depthwise block with per-sample normalization."""

    def __init__(
        self,
        channels: int,
        *,
        large_kernel: int,
        small_kernel: int,
        ffn_ratio: int,
        dropout: float,
        causal: bool = False,
        dilation: int = 1,
    ) -> None:
        super().__init__()
        expanded = channels * ffn_ratio
        self.causal = bool(causal)
        self.large_kernel = int(large_kernel)
        self.small_kernel = int(small_kernel)
        self.dilation = int(dilation)
        if self.dilation <= 0:
            raise ValueError("dilation must be positive")
        self.dw_large = nn.Conv1d(
            channels,
            channels,
            large_kernel,
            padding=0 if self.causal else self.dilation * (large_kernel // 2),
            dilation=self.dilation,
            groups=channels,
            bias=False,
        )
        self.dw_small = nn.Conv1d(
            channels,
            channels,
            small_kernel,
            padding=0 if self.causal else self.dilation * (small_kernel // 2),
            dilation=self.dilation,
            groups=channels,
            bias=False,
        )
        self.depthwise_norm = (
            ChannelLayerNorm1d(channels) if self.causal else nn.GroupNorm(1, channels)
        )
        self.ffn = nn.Sequential(
            nn.Conv1d(channels, expanded, 1),
            nn.GELU(),
            nn.Dropout(dropout),
            nn.Conv1d(expanded, channels, 1),
        )
        self.ffn_norm = (
            ChannelLayerNorm1d(channels) if self.causal else nn.GroupNorm(1, channels)
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        if self.causal:
            large = self.dw_large(
                torch.nn.functional.pad(
                    x, ((self.large_kernel - 1) * self.dilation, 0)
                )
            )
            small = self.dw_small(
                torch.nn.functional.pad(
                    x, ((self.small_kernel - 1) * self.dilation, 0)
                )
            )
        else:
            large = self.dw_large(x)
            small = self.dw_small(x)
        x = x + self.depthwise_norm(large + small)
        return torch.nn.functional.gelu(x + self.ffn_norm(self.ffn(x)))


class ChannelLayerNorm1d(nn.Module):
    """Layer-normalize channels independently at every time step."""

    def __init__(self, channels: int) -> None:
        super().__init__()
        self.norm = nn.LayerNorm(channels)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.norm(x.transpose(1, 2)).transpose(1, 2)


class AnonymousRelationBlock(nn.Module):
    """Permutation-equivariant interaction between anonymous channel groups."""

    def __init__(self, channels: int, dropout: float, *, causal: bool = False) -> None:
        super().__init__()
        self.projection = nn.Sequential(
            nn.Conv1d(channels * 4, channels * 2, 1),
            nn.GELU(),
            nn.Dropout(dropout),
            nn.Conv1d(channels * 2, channels, 1),
        )
        self.norm = ChannelLayerNorm1d(channels) if causal else nn.GroupNorm(1, channels)

    def forward(self, hidden: torch.Tensor, mask: torch.Tensor) -> torch.Tensor:
        batch, groups, channels, time = hidden.shape
        expanded_mask = mask[:, :, None, None]
        mean = (hidden * expanded_mask).sum(dim=1, keepdim=True) / expanded_mask.sum(
            dim=1, keepdim=True
        ).clamp_min(1)
        maximum = hidden.masked_fill(
            ~expanded_mask, torch.finfo(hidden.dtype).min
        ).amax(dim=1, keepdim=True)
        context = torch.cat(
            [
                hidden,
                hidden - mean,
                mean.expand(-1, groups, -1, -1),
                maximum.expand(-1, groups, -1, -1),
            ],
            dim=2,
        )
        update = self.projection(context.reshape(batch * groups, channels * 4, time))
        update = self.norm(update).reshape(batch, groups, channels, time)
        return torch.nn.functional.gelu(hidden + update) * expanded_mask


class ModalityResidualAdapter(nn.Module):
    """Small zero-initialized residual expert selected only by data modality."""

    def __init__(self, channels: int, rank: int, dropout: float) -> None:
        super().__init__()
        self.down = nn.Conv1d(channels, rank, 1)
        self.dropout = nn.Dropout(dropout)
        self.up = nn.Conv1d(rank, channels, 1)
        nn.init.zeros_(self.up.weight)
        nn.init.zeros_(self.up.bias)

    def forward(self, hidden: torch.Tensor) -> torch.Tensor:
        return self.up(self.dropout(torch.nn.functional.gelu(self.down(hidden))))


class AnonymousModernTCN(nn.Module):
    """Permutation-invariant ModernTCN accepting one to sixteen anonymous groups.

    Input is `(batch, groups, features, time)`. No channel number, name, or
    group-specific trainable weight is present in the module.
    """

    def __init__(self, config: AnonymousModernTCNConfig) -> None:
        super().__init__()
        if config.feature_dim <= 0 or config.hidden_channels <= 0:
            raise ValueError("feature_dim and hidden_channels must be positive")
        if config.dual_view_stem and config.feature_dim % 2:
            raise ValueError("dual_view_stem requires an even feature_dim")
        if config.temporal_dilation_cycle <= 0:
            raise ValueError("temporal_dilation_cycle must be positive")
        if config.category_head_count not in {0, 2, 3}:
            raise ValueError("category_head_count must be 0, 2, or 3")
        if config.group_union_output and not config.all_member_pooling:
            raise ValueError("group_union_output requires all_member_pooling")
        if config.group_union_output and config.category_head_count:
            raise ValueError("group_union_output cannot be combined with category heads")
        if config.window_tail_pool_steps < 0:
            raise ValueError("window_tail_pool_steps must be non-negative")
        if config.dual_view_contrast and not config.dual_view_stem:
            raise ValueError("dual_view_contrast requires dual_view_stem")
        if config.long_context_features < 0:
            raise ValueError("long_context_features must be non-negative")
        if config.long_context_features and config.dual_view_stem:
            raise ValueError("long_context_features and dual_view_stem are alternative stems")
        if config.long_context_features >= config.feature_dim:
            raise ValueError("long_context_features must leave short-context features")
        if config.prewindow_level_contrast and not config.long_context_features:
            raise ValueError(
                "prewindow_level_contrast requires long_context_features"
            )
        if (
            config.sustained_prewindow_level_contrast
            and not config.long_context_features
        ):
            raise ValueError(
                "sustained_prewindow_level_contrast requires long_context_features"
            )
        if (
            config.sustained_prewindow_level_contrast
            and not config.sustained_direct_input
        ):
            raise ValueError(
                "sustained_prewindow_level_contrast requires sustained_direct_input"
            )
        if (
            config.member_max_clip_sigma < 0.0
            or config.sustained_member_max_clip_sigma < 0.0
            or config.group_max_clip_sigma < 0.0
        ):
            raise ValueError("robust pooling sigma values must be non-negative")
        if (
            config.sustained_member_max_clip_sigma > 0.0
            and not config.sustained_direct_input
        ):
            raise ValueError(
                "sustained_member_max_clip_sigma requires sustained_direct_input"
            )
        if config.modality_adapter_count < 0:
            raise ValueError("modality_adapter_count must be non-negative")
        if config.modality_adapter_rank <= 0:
            raise ValueError("modality_adapter_rank must be positive")
        if config.temporal_modality_adapters and config.modality_adapter_count <= 0:
            raise ValueError(
                "temporal_modality_adapters require modality adapters"
            )
        if config.modality_logit_calibration and config.modality_adapter_count <= 0:
            raise ValueError(
                "modality_logit_calibration requires modality adapters"
            )
        if (
            config.channel_blocks < 0
            or config.relation_blocks < 0
            or config.temporal_blocks <= 0
            or config.short_temporal_blocks < 0
            or config.member_temporal_blocks < 0
        ):
            raise ValueError("model block counts are invalid")
        if config.member_temporal_blocks and not config.member_head:
            raise ValueError("member_temporal_blocks require member_head")
        if config.member_temporal_residual and not config.member_temporal_blocks:
            raise ValueError("member_temporal_residual requires member temporal blocks")
        if config.member_peer_context and not config.member_head:
            raise ValueError("member_peer_context requires member_head")
        if config.recovery_head and not config.all_member_pooling:
            raise ValueError("recovery_head requires all_member_pooling")
        if config.sustained_head and not config.all_member_pooling:
            raise ValueError("sustained_head requires all_member_pooling")
        if config.sustained_direct_input and not config.sustained_head:
            raise ValueError("sustained_direct_input requires sustained_head")
        if not 1 <= config.max_groups <= 16:
            raise ValueError("max_groups must be in [1, 16]")
        if (
            config.large_kernel <= 0
            or config.small_kernel <= 0
            or config.short_kernel <= 0
            or config.large_kernel % 2 == 0
            or config.small_kernel % 2 == 0
            or config.short_kernel % 2 == 0
        ):
            raise ValueError("kernel sizes must be odd")
        self.config = config
        short_feature_dim = config.feature_dim - config.long_context_features
        self.shared_stem = (
            None
            if config.dual_view_stem
            else nn.Conv1d(short_feature_dim, config.hidden_channels, 1)
        )
        view_features = config.feature_dim // 2
        self.nominal_stem = (
            nn.Conv1d(view_features, config.hidden_channels, 1)
            if config.dual_view_stem
            else None
        )
        self.causal_stem = (
            nn.Conv1d(view_features, config.hidden_channels, 1)
            if config.dual_view_stem
            else None
        )
        self.view_fusion = (
            nn.Conv1d(
                config.hidden_channels * (3 if config.dual_view_contrast else 2),
                config.hidden_channels,
                1,
            )
            if config.dual_view_stem
            else None
        )
        self.long_context_stem = (
            nn.Conv1d(
                config.long_context_features,
                config.hidden_channels,
                1,
            )
            if config.long_context_features
            else None
        )
        self.long_context_fusion = (
            nn.Conv1d(config.hidden_channels * 3, config.hidden_channels, 1)
            if config.long_context_features
            else None
        )
        if self.long_context_fusion is not None:
            # Start as an exact short-view identity. Fine-tuning can then add
            # long-view evidence without destroying a compatible short model.
            self.reset_long_context_fusion()
        self.channel_blocks = nn.ModuleList(
            [self._block() for _ in range(config.channel_blocks)]
        )
        self.modality_adapters = nn.ModuleList(
            [
                ModalityResidualAdapter(
                    config.hidden_channels,
                    config.modality_adapter_rank,
                    config.dropout,
                )
                for _ in range(config.modality_adapter_count)
            ]
        )
        self.temporal_modality_adapters = nn.ModuleList(
            [
                ModalityResidualAdapter(
                    config.hidden_channels,
                    config.modality_adapter_rank,
                    config.dropout,
                )
                for _ in range(
                    config.modality_adapter_count
                    if config.temporal_modality_adapters
                    else 0
                )
            ]
        )
        self.modality_log_scale = (
            nn.Parameter(torch.zeros(config.modality_adapter_count))
            if config.modality_logit_calibration
            else None
        )
        self.modality_logit_bias = (
            nn.Parameter(torch.zeros(config.modality_adapter_count))
            if config.modality_logit_calibration
            else None
        )
        self.relation_blocks = nn.ModuleList(
            [
                AnonymousRelationBlock(
                    config.hidden_channels,
                    config.dropout,
                    causal=config.causal_convolutions,
                )
                for _ in range(config.relation_blocks)
            ]
        )
        self.member_fusion = (
            nn.Conv1d(config.hidden_channels * 3, config.hidden_channels, 1)
            if config.all_member_pooling
            else None
        )
        self.member_sequence_head = (
            nn.Conv1d(config.hidden_channels, 1, 1)
            if config.all_member_pooling and config.member_head
            else None
        )
        self.member_temporal_blocks = nn.ModuleList(
            [
                self._block(
                    dilation=2 ** (index % config.temporal_dilation_cycle)
                )
                for index in range(config.member_temporal_blocks)
            ]
        )
        self.member_temporal_fusion = (
            nn.Conv1d(config.hidden_channels, config.hidden_channels, 1)
            if config.member_temporal_residual
            else None
        )
        if self.member_temporal_fusion is not None:
            nn.init.zeros_(self.member_temporal_fusion.weight)
            nn.init.zeros_(self.member_temporal_fusion.bias)
        self.member_peer_fusion = (
            nn.Conv1d(config.hidden_channels * 3, config.hidden_channels, 1)
            if config.member_peer_context
            else None
        )
        if self.member_peer_fusion is not None:
            nn.init.zeros_(self.member_peer_fusion.weight)
            nn.init.zeros_(self.member_peer_fusion.bias)
        self.group_sequence_head = (
            nn.Conv1d(config.hidden_channels, 1, 1)
            if config.all_member_pooling
            else None
        )
        self.group_window_head = (
            nn.Linear(config.hidden_channels * 2, 1)
            if config.all_member_pooling
            else None
        )
        self.channel_fusion = nn.Conv1d(config.hidden_channels * 2, config.hidden_channels, 1)
        self.temporal_blocks = nn.ModuleList(
            [
                self._block(
                    dilation=2 ** (index % config.temporal_dilation_cycle)
                )
                for index in range(config.temporal_blocks)
            ]
        )
        self.short_temporal_blocks = nn.ModuleList(
            [self._short_block() for _ in range(config.short_temporal_blocks)]
        )
        self.temporal_fusion = (
            nn.Conv1d(config.hidden_channels * 2, config.hidden_channels, 1)
            if config.short_temporal_blocks
            else None
        )
        self.sequence_head = nn.Conv1d(config.hidden_channels, 1, 1)
        self.category_sequence_head = (
            nn.Conv1d(config.hidden_channels, config.category_head_count, 1)
            if config.category_head_count
            else None
        )
        self.recovery_sequence_head = (
            nn.Conv1d(config.hidden_channels, 1, 1)
            if config.recovery_head
            else None
        )
        self.sustained_window_head = (
            nn.Linear(config.hidden_channels * 2, 1)
            if config.sustained_head and not config.sustained_direct_input
            else None
        )
        sustained_rank = max(8, min(64, config.hidden_channels // 2))
        self.sustained_direct_stem = (
            nn.Conv1d(config.feature_dim, sustained_rank, 1)
            if config.sustained_direct_input
            else None
        )
        self.sustained_direct_head = (
            nn.Sequential(
                nn.Linear(sustained_rank * 4, sustained_rank * 2),
                nn.GELU(),
                nn.Dropout(config.dropout),
                nn.Linear(sustained_rank * 2, 1),
            )
            if config.sustained_direct_input
            else None
        )
        self.window_head = nn.Linear(config.hidden_channels * 2, 1)

    def reset_long_context_fusion(self) -> None:
        """Ignore the long view initially while keeping its branch trainable."""

        if self.long_context_fusion is None:
            raise RuntimeError("model has no long-context fusion")
        nn.init.zeros_(self.long_context_fusion.weight)
        nn.init.zeros_(self.long_context_fusion.bias)
        with torch.no_grad():
            diagonal = torch.arange(self.config.hidden_channels)
            self.long_context_fusion.weight[diagonal, diagonal, 0] = 1.0

    def _block(self, *, dilation: int = 1) -> ModernTCNBlock:
        return ModernTCNBlock(
            self.config.hidden_channels,
            large_kernel=self.config.large_kernel,
            small_kernel=self.config.small_kernel,
            ffn_ratio=self.config.ffn_ratio,
            dropout=self.config.dropout,
            causal=self.config.causal_convolutions,
            dilation=dilation,
        )

    def _short_block(self) -> ModernTCNBlock:
        return ModernTCNBlock(
            self.config.hidden_channels,
            large_kernel=self.config.short_kernel,
            small_kernel=self.config.small_kernel,
            ffn_ratio=self.config.ffn_ratio,
            dropout=self.config.dropout,
            causal=self.config.causal_convolutions,
        )
    def forward(
        self,
        x: torch.Tensor,
        group_mask: torch.Tensor | None = None,
        *,
        modality_ids: torch.Tensor | None = None,
    ) -> tuple[torch.Tensor, torch.Tensor]:
        if x.ndim != 4:
            raise ValueError("x must be shaped (batch, groups, features, time)")
        batch, groups, features, time = x.shape
        if groups > self.config.max_groups:
            raise ValueError(f"active/padded groups exceed max_groups={self.config.max_groups}")
        if features != self.config.feature_dim:
            raise ValueError("input feature count does not match model configuration")
        if group_mask is None:
            group_mask = torch.ones((batch, groups), dtype=torch.bool, device=x.device)
        if group_mask.shape != (batch, groups):
            raise ValueError("group_mask must be shaped (batch, groups)")
        group_mask = group_mask.to(dtype=torch.bool, device=x.device)
        if bool((group_mask.sum(dim=1) == 0).any()):
            raise ValueError("every sample must contain at least one active group")

        hidden = self._shared_encode(x, modality_ids=modality_ids)
        for block in self.relation_blocks:
            hidden = block(hidden, group_mask)
        (
            window_logits,
            sequence_logits,
            _category_logits,
            _recovery_logits,
            _sustained_logits,
            _embedding,
        ) = self._global_forward(
            hidden, group_mask, modality_ids=modality_ids
        )
        window_logits = self._calibrate_modality_logits(
            window_logits, modality_ids
        )
        sequence_logits = self._calibrate_modality_logits(
            sequence_logits, modality_ids
        )
        return window_logits, sequence_logits

    def forward_all_members(
        self,
        x: torch.Tensor,
        channel_mask: torch.Tensor,
        group_membership: torch.Tensor,
        group_mask: torch.Tensor,
        *,
        modality_ids: torch.Tensor | None = None,
    ) -> AnonymousModernTCNOutput:
        """Encode every physical channel, then pool members inside each group."""

        if not self.config.all_member_pooling or self.member_fusion is None:
            raise RuntimeError("model was not configured for all-member pooling")
        if x.ndim != 4:
            raise ValueError("x must be shaped (batch, channels, features, time)")
        batch, channels, features, time = x.shape
        if features != self.config.feature_dim:
            raise ValueError("input feature count does not match model configuration")
        if channel_mask.shape != (batch, channels):
            raise ValueError("channel_mask must be shaped (batch, channels)")
        if group_membership.ndim != 3 or group_membership.shape[0] != batch:
            raise ValueError("group_membership must be shaped (batch, groups, channels)")
        groups = group_membership.shape[1]
        if group_membership.shape[2] != channels or group_mask.shape != (batch, groups):
            raise ValueError("group membership/mask shapes are incompatible")
        if groups > self.config.max_groups:
            raise ValueError(f"active/padded groups exceed max_groups={self.config.max_groups}")
        channel_mask = channel_mask.to(dtype=torch.bool, device=x.device)
        group_mask = group_mask.to(dtype=torch.bool, device=x.device)
        membership = group_membership.to(dtype=torch.bool, device=x.device)
        membership = membership & channel_mask[:, None, :]
        if bool((membership.sum(dim=1) > 1).any()):
            raise ValueError("a physical channel cannot belong to multiple groups")
        channel_mask &= membership.any(dim=1)
        counts = membership.sum(dim=2)
        if bool(((counts == 0) & group_mask).any()):
            raise ValueError("every active group must contain an active physical channel")
        if bool((group_mask.sum(dim=1) == 0).any()):
            raise ValueError("every sample must contain at least one active group")

        member_hidden = self._shared_encode(
            x, modality_ids=modality_ids
        ) * channel_mask[:, :, None, None]
        member_sequence_logits = self._member_logits_from_hidden(
            self._member_peer_hidden(
                member_hidden,
                channel_mask,
                membership,
            )
        )
        weights = membership.to(member_hidden.dtype)
        divisor = counts.clamp_min(1).to(member_hidden.dtype)[:, :, None, None]
        mean = torch.einsum("bgc,bcht->bght", weights, member_hidden) / divisor
        second = torch.einsum(
            "bgc,bcht->bght", weights, member_hidden.square()
        ) / divisor
        std = torch.sqrt(torch.clamp(second - mean.square(), min=0.0) + 1e-6)
        minimum = torch.finfo(member_hidden.dtype).min
        channel_to_group = membership.to(dtype=torch.int64).argmax(dim=1)
        maximum = torch.full(
            (batch, groups, self.config.hidden_channels, time),
            minimum,
            dtype=member_hidden.dtype,
            device=member_hidden.device,
        )
        maximum.scatter_reduce_(
            1,
            channel_to_group[:, :, None, None].expand(
                -1, -1, self.config.hidden_channels, time
            ),
            member_hidden.masked_fill(~channel_mask[:, :, None, None], minimum),
            reduce="amax",
            include_self=True,
        )
        maximum = maximum.masked_fill(~group_mask[:, :, None, None], 0.0)
        maximum = clip_anonymous_maximum(
            maximum,
            mean,
            std,
            sigma=self.config.member_max_clip_sigma,
        )
        pooled = torch.cat([mean, maximum, std], dim=2)
        group_hidden = self.member_fusion(
            pooled.reshape(batch * groups, self.config.hidden_channels * 3, time)
        ).reshape(batch, groups, self.config.hidden_channels, time)
        group_hidden = torch.nn.functional.gelu(group_hidden).masked_fill(
            ~group_mask[:, :, None, None], 0.0
        )
        for block in self.relation_blocks:
            group_hidden = block(group_hidden, group_mask)
        if self.group_sequence_head is None or self.group_window_head is None:
            raise RuntimeError("all-member group heads are missing")
        group_sequence_logits = self.group_sequence_head(
            group_hidden.reshape(batch * groups, self.config.hidden_channels, time)
        ).reshape(batch, groups, time)
        group_temporal = torch.cat(
            [group_hidden.mean(dim=-1), group_hidden.amax(dim=-1)], dim=2
        )
        group_window_logits = self.group_window_head(
            group_temporal.reshape(batch * groups, self.config.hidden_channels * 2)
        ).reshape(batch, groups)
        (
            window_logits,
            sequence_logits,
            category_logits,
            recovery_logits,
            sustained_logits,
            embedding,
        ) = self._global_forward(
            group_hidden, group_mask, modality_ids=modality_ids
        )
        if self.config.sustained_direct_input:
            sustained_logits = self._direct_sustained_forward(x, channel_mask)
        if self.config.group_union_output:
            sequence_logits = group_sequence_logits.masked_fill(
                ~group_mask[:, :, None], torch.finfo(group_sequence_logits.dtype).min
            ).amax(dim=1)
            window_logits = group_window_logits.masked_fill(
                ~group_mask, torch.finfo(group_window_logits.dtype).min
            ).amax(dim=1)
        window_logits = self._calibrate_modality_logits(
            window_logits, modality_ids
        )
        sequence_logits = self._calibrate_modality_logits(
            sequence_logits, modality_ids
        )
        group_window_logits = self._calibrate_modality_logits(
            group_window_logits, modality_ids
        )
        group_sequence_logits = self._calibrate_modality_logits(
            group_sequence_logits, modality_ids
        )
        if member_sequence_logits is not None:
            member_sequence_logits = self._calibrate_modality_logits(
                member_sequence_logits, modality_ids
            )
            member_sequence_logits = member_sequence_logits.masked_fill(
                ~channel_mask[:, :, None],
                torch.finfo(member_sequence_logits.dtype).min,
            )
        if recovery_logits is not None:
            recovery_logits = self._calibrate_modality_logits(
                recovery_logits, modality_ids
            )
        if sustained_logits is not None:
            sustained_logits = self._calibrate_modality_logits(
                sustained_logits, modality_ids
            )
        return AnonymousModernTCNOutput(
            window_logits=window_logits,
            sequence_logits=sequence_logits,
            group_window_logits=group_window_logits,
            group_sequence_logits=group_sequence_logits,
            category_sequence_logits=category_logits,
            recovery_sequence_logits=recovery_logits,
            sustained_window_logits=sustained_logits,
            window_embedding=embedding,
            member_sequence_logits=member_sequence_logits,
        )

    def _direct_sustained_forward(
        self,
        x: torch.Tensor,
        channel_mask: torch.Tensor,
    ) -> torch.Tensor:
        """Score stable anomaly evidence without channel identity or transitions."""

        if self.sustained_direct_stem is None or self.sustained_direct_head is None:
            raise RuntimeError("direct sustained branch is not configured")
        x = self._prewindow_contrast_input(
            x,
            enabled=(
                self.config.prewindow_level_contrast
                or self.config.sustained_prewindow_level_contrast
            ),
        )
        batch, channels, features, time = x.shape
        hidden = torch.nn.functional.gelu(
            self.sustained_direct_stem(
                x.reshape(batch * channels, features, time)
            )
        )
        rank = hidden.shape[1]
        per_channel = torch.cat(
            [hidden.mean(dim=-1), hidden.amax(dim=-1)], dim=1
        ).reshape(batch, channels, rank * 2)
        mask = channel_mask[:, :, None]
        mean = (per_channel * mask).sum(dim=1) / mask.sum(dim=1).clamp_min(1)
        second = (
            per_channel.square() * mask
        ).sum(dim=1) / mask.sum(dim=1).clamp_min(1)
        std = torch.sqrt(torch.clamp(second - mean.square(), min=0.0) + 1e-6)
        maximum = per_channel.masked_fill(
            ~mask, torch.finfo(per_channel.dtype).min
        ).amax(dim=1)
        maximum = clip_anonymous_maximum(
            maximum,
            mean,
            std,
            sigma=max(
                self.config.member_max_clip_sigma,
                self.config.sustained_member_max_clip_sigma,
            ),
        )
        pooled = torch.cat([mean, maximum], dim=1)
        return self.sustained_direct_head(pooled).squeeze(1)

    def forward_member_sequences(
        self,
        x: torch.Tensor,
        member_mask: torch.Tensor | None = None,
        group_membership: torch.Tensor | None = None,
        *,
        modality_ids: torch.Tensor | None = None,
    ) -> torch.Tensor:
        """Score anonymous physical channels before group/global pooling.

        The same encoder and temporal branch are applied to every member, so
        this path remains equivariant to channel permutation.  It also allows
        training on a balanced subset of members without evaluating the much
        more expensive global all-channel path.
        """

        if self.member_sequence_head is None:
            raise RuntimeError("model was not configured with a member head")
        if x.ndim != 4:
            raise ValueError("x must be shaped (batch, members, features, time)")
        batch, members, _features, time = x.shape
        if member_mask is None:
            member_mask = torch.ones(
                (batch, members), dtype=torch.bool, device=x.device
            )
        if member_mask.shape != (batch, members):
            raise ValueError("member_mask must be shaped (batch, members)")
        member_mask = member_mask.to(dtype=torch.bool, device=x.device)
        hidden = self._shared_encode(x, modality_ids=modality_ids)
        if self.member_peer_fusion is not None:
            if group_membership is None:
                raise ValueError(
                    "member peer context requires group_membership"
                )
            hidden = self._member_peer_hidden(
                hidden,
                member_mask,
                group_membership.to(dtype=torch.bool, device=x.device),
            )
        logits = self._member_logits_from_hidden(hidden)
        if logits is None:
            raise RuntimeError("member head output is missing")
        return logits.masked_fill(
            ~member_mask[:, :, None], torch.finfo(logits.dtype).min
        ).reshape(batch, members, time)

    def _member_peer_hidden(
        self,
        member_hidden: torch.Tensor,
        member_mask: torch.Tensor,
        group_membership: torch.Tensor,
    ) -> torch.Tensor:
        """Add leave-one-out anonymous peer context without changing pooling."""

        if self.member_peer_fusion is None:
            return member_hidden
        batch, members, channels, time = member_hidden.shape
        if member_mask.shape != (batch, members):
            raise ValueError("member_mask must align with member hidden")
        if (
            group_membership.ndim != 3
            or group_membership.shape[0] != batch
            or group_membership.shape[2] != members
        ):
            raise ValueError(
                "group_membership must be shaped (batch, groups, members)"
            )
        membership = group_membership & member_mask[:, None, :]
        if bool((membership.sum(dim=1) > 1).any()):
            raise ValueError("a member cannot belong to multiple peer groups")
        weights = membership.to(member_hidden.dtype)
        group_sum = torch.einsum("bgm,bmht->bght", weights, member_hidden)
        group_count = membership.sum(dim=2)
        member_group = membership.to(torch.int64).argmax(dim=1)
        gather_index = member_group[:, :, None, None].expand(
            -1, -1, channels, time
        )
        selected_sum = torch.gather(group_sum, 1, gather_index)
        selected_count = torch.gather(group_count, 1, member_group).to(
            member_hidden.dtype
        )[:, :, None, None]
        peer_count = selected_count - 1.0
        peer_mean = torch.where(
            peer_count > 0,
            (selected_sum - member_hidden) / peer_count.clamp_min(1.0),
            member_hidden,
        )
        peer_mean = peer_mean * member_mask[:, :, None, None]
        context = torch.cat(
            [member_hidden, peer_mean, member_hidden - peer_mean], dim=2
        )
        update = self.member_peer_fusion(
            context.reshape(batch * members, channels * 3, time)
        ).reshape(batch, members, channels, time)
        return (member_hidden + update) * member_mask[:, :, None, None]

    def _member_logits_from_hidden(
        self, member_hidden: torch.Tensor
    ) -> torch.Tensor | None:
        if self.member_sequence_head is None:
            return None
        batch, members, channels, time = member_hidden.shape
        hidden = member_hidden.reshape(batch * members, channels, time)
        base = hidden
        for block in self.member_temporal_blocks:
            hidden = block(hidden)
        if self.member_temporal_fusion is not None:
            hidden = base + self.member_temporal_fusion(hidden - base)
        return self.member_sequence_head(hidden).reshape(batch, members, time)

    def _shared_encode(
        self,
        x: torch.Tensor,
        *,
        modality_ids: torch.Tensor | None = None,
    ) -> torch.Tensor:
        x = self._prewindow_contrast_input(x)
        batch, items, features, time = x.shape
        modality_ids = self._normalized_modality_ids(
            batch, modality_ids, x.device
        )
        flattened = x.reshape(batch * items, features, time)
        if self.config.dual_view_stem:
            if (
                self.nominal_stem is None
                or self.causal_stem is None
                or self.view_fusion is None
            ):
                raise RuntimeError("dual-view stem modules are missing")
            midpoint = features // 2
            nominal = self.nominal_stem(flattened[:, :midpoint])
            causal = self.causal_stem(flattened[:, midpoint:])
            views = [nominal, causal]
            if self.config.dual_view_contrast:
                views.append(nominal - causal)
            hidden = torch.nn.functional.gelu(
                self.view_fusion(torch.cat(views, dim=1))
            )
        else:
            if self.shared_stem is None:
                raise RuntimeError("shared stem is missing")
            short_feature_count = features - self.config.long_context_features
            hidden = self.shared_stem(flattened[:, :short_feature_count])
            if self.config.long_context_features:
                if self.long_context_stem is None or self.long_context_fusion is None:
                    raise RuntimeError("long-context stem modules are missing")
                long_hidden = self.long_context_stem(flattened[:, short_feature_count:])
                hidden = self.long_context_fusion(
                    torch.cat([hidden, long_hidden, hidden - long_hidden], dim=1)
                )
        for block in self.channel_blocks:
            hidden = block(hidden)
        if self.modality_adapters:
            item_modalities = modality_ids[:, None].expand(-1, items).reshape(-1)
            hidden = self._apply_modality_adapters(
                hidden, item_modalities, self.modality_adapters
            )
        return hidden.reshape(batch, items, self.config.hidden_channels, time)

    def _prewindow_contrast_input(
        self,
        x: torch.Tensor,
        *,
        enabled: bool | None = None,
    ) -> torch.Tensor:
        """Remove anonymous level drift while retaining pre-window change.

        The first short and long planes are raw values in the same global
        normalization. Subtracting the long-view median from both removes a
        channel-wide operating-level shift. The short plane still contains the
        causal current-vs-prewindow contrast needed for fully anomalous windows.
        """

        if enabled is None:
            enabled = self.config.prewindow_level_contrast
        if not enabled:
            return x
        if x.ndim != 4:
            raise ValueError("x must be shaped (batch, items, features, time)")
        long_left = x.shape[2] - self.config.long_context_features
        reference = x[:, :, long_left, :].median(dim=-1, keepdim=True).values
        contrasted = x.clone()
        contrasted[:, :, 0, :] = contrasted[:, :, 0, :] - reference
        contrasted[:, :, long_left, :] = (
            contrasted[:, :, long_left, :] - reference
        )
        return contrasted

    def _normalized_modality_ids(
        self,
        batch: int,
        modality_ids: torch.Tensor | None,
        device: torch.device,
    ) -> torch.Tensor:
        if modality_ids is None:
            modality_ids = torch.zeros(batch, dtype=torch.long, device=device)
        if modality_ids.shape != (batch,):
            raise ValueError("modality_ids must be shaped (batch,)")
        modality_ids = modality_ids.to(device=device, dtype=torch.long)
        if self.config.modality_adapter_count == 0:
            if bool((modality_ids != 0).any()):
                raise ValueError("model has no modality adapters")
        elif bool(
            (
                (modality_ids < 0)
                | (modality_ids >= self.config.modality_adapter_count)
            ).any()
        ):
            raise ValueError("modality_ids exceed configured adapter count")
        return modality_ids

    @staticmethod
    def _apply_modality_adapters(
        hidden: torch.Tensor,
        modality_ids: torch.Tensor,
        adapters: nn.ModuleList,
    ) -> torch.Tensor:
        update = torch.zeros_like(hidden)
        for modality, adapter in enumerate(adapters):
            selected = modality_ids == modality
            if bool(selected.any()):
                update[selected] = adapter(hidden[selected])
        return hidden + update

    def _calibrate_modality_logits(
        self,
        logits: torch.Tensor,
        modality_ids: torch.Tensor | None,
    ) -> torch.Tensor:
        if self.modality_log_scale is None or self.modality_logit_bias is None:
            return logits
        selected = self._normalized_modality_ids(
            logits.shape[0], modality_ids, logits.device
        )
        shape = (logits.shape[0],) + (1,) * (logits.ndim - 1)
        scale = torch.exp(self.modality_log_scale[selected]).reshape(shape)
        bias = self.modality_logit_bias[selected].reshape(shape)
        return logits * scale + bias

    def _global_forward(
        self,
        hidden: torch.Tensor,
        group_mask: torch.Tensor,
        *,
        modality_ids: torch.Tensor | None = None,
    ) -> tuple[
        torch.Tensor,
        torch.Tensor,
        torch.Tensor | None,
        torch.Tensor | None,
        torch.Tensor | None,
        torch.Tensor,
    ]:
        mask = group_mask[:, :, None, None]
        masked = hidden * mask
        mean = masked.sum(dim=1) / mask.sum(dim=1).clamp_min(1)
        second = (hidden.square() * mask).sum(dim=1) / mask.sum(dim=1).clamp_min(1)
        std = torch.sqrt(torch.clamp(second - mean.square(), min=0.0) + 1e-6)
        maximum = hidden.masked_fill(~mask, torch.finfo(hidden.dtype).min).amax(dim=1)
        maximum = clip_anonymous_maximum(
            maximum,
            mean,
            std,
            sigma=self.config.group_max_clip_sigma,
        )
        fused = torch.nn.functional.gelu(self.channel_fusion(torch.cat([mean, maximum], dim=1)))
        if self.temporal_modality_adapters:
            selected_modalities = self._normalized_modality_ids(
                fused.shape[0], modality_ids, fused.device
            )
            fused = self._apply_modality_adapters(
                fused,
                selected_modalities,
                self.temporal_modality_adapters,
            )
        short = fused
        for block in self.temporal_blocks:
            fused = block(fused)
        if self.short_temporal_blocks:
            for block in self.short_temporal_blocks:
                short = block(short)
            if self.temporal_fusion is None:
                raise RuntimeError("short temporal path is missing its fusion layer")
            fused = torch.nn.functional.gelu(
                self.temporal_fusion(torch.cat([fused, short], dim=1))
            )
        sequence_logits = self.sequence_head(fused).squeeze(1)
        category_logits = (
            self.category_sequence_head(fused)
            if self.category_sequence_head is not None
            else None
        )
        if category_logits is not None and self.config.category_union_output:
            sequence_logits = category_union_logits(category_logits)
        recovery_logits = (
            self.recovery_sequence_head(fused).squeeze(1)
            if self.recovery_sequence_head is not None
            else None
        )
        if self.config.window_tail_pool_steps:
            horizon = min(self.config.window_tail_pool_steps, fused.shape[-1])
            pooled = torch.cat(
                [fused[..., -1], fused[..., -horizon:].amax(dim=-1)], dim=1
            )
        else:
            pooled = torch.cat([fused.mean(dim=-1), fused.amax(dim=-1)], dim=1)
        window_logits = self.window_head(pooled).squeeze(1)
        sustained_logits = (
            self.sustained_window_head(pooled).squeeze(1)
            if self.sustained_window_head is not None
            else None
        )
        return (
            window_logits,
            sequence_logits,
            category_logits,
            recovery_logits,
            sustained_logits,
            fused[..., -1],
        )


def category_union_logits(category_logits: torch.Tensor) -> torch.Tensor:
    """Convert independent category logits to the logit of their probability union."""

    if category_logits.ndim < 2 or category_logits.shape[1] <= 0:
        raise ValueError("category_logits must contain a category dimension")
    log_not_anomaly = torch.nn.functional.logsigmoid(-category_logits).sum(dim=1)
    union_probability = -torch.expm1(log_not_anomaly)
    tiny = torch.finfo(category_logits.dtype).tiny
    return torch.log(union_probability.clamp_min(tiny)) - log_not_anomaly


def clip_anonymous_maximum(
    maximum: torch.Tensor,
    mean: torch.Tensor,
    std: torch.Tensor,
    *,
    sigma: float,
) -> torch.Tensor:
    """Winsorize anonymous max evidence without dropping any input member."""

    if sigma < 0.0:
        raise ValueError("sigma must be non-negative")
    if maximum.shape != mean.shape or maximum.shape != std.shape:
        raise ValueError("maximum, mean, and std shapes must match")
    if sigma == 0.0:
        return maximum
    return torch.minimum(maximum, mean + float(sigma) * std)


def effective_receptive_field(config: AnonymousModernTCNConfig) -> int:
    """Maximum long-path receptive field in input time steps."""
    temporal_dilations = (
        2 ** (index % config.temporal_dilation_cycle)
        for index in range(config.temporal_blocks)
    )
    return 1 + (config.large_kernel - 1) * (
        config.channel_blocks + sum(temporal_dilations)
    )


def apply_group_dropout(
    x: torch.Tensor,
    group_mask: torch.Tensor,
    probability: float,
    *,
    generator: torch.Generator | None = None,
) -> tuple[torch.Tensor, torch.Tensor]:
    if not 0.0 <= probability < 1.0:
        raise ValueError("probability must be in [0, 1)")
    if probability == 0.0:
        return x, group_mask
    keep = torch.rand(group_mask.shape, device=group_mask.device, generator=generator) >= probability
    keep &= group_mask
    for row in range(keep.shape[0]):
        if not bool(keep[row].any()):
            first = torch.nonzero(group_mask[row], as_tuple=False)[0, 0]
            keep[row, first] = True
    return x * keep[:, :, None, None], keep


def apply_member_dropout(
    x: torch.Tensor,
    channel_mask: torch.Tensor,
    group_membership: torch.Tensor,
    group_mask: torch.Tensor,
    probability: float,
    *,
    generator: torch.Generator | None = None,
) -> tuple[torch.Tensor, torch.Tensor]:
    """Drop physical members while retaining one member in every active group."""

    if not 0.0 <= probability < 1.0:
        raise ValueError("probability must be in [0, 1)")
    if probability == 0.0:
        return x, channel_mask
    keep = (
        torch.rand(channel_mask.shape, device=x.device, generator=generator)
        >= probability
    ) & channel_mask
    membership = group_membership.to(dtype=torch.bool, device=x.device)
    for batch_index in range(x.shape[0]):
        for group_index in torch.nonzero(group_mask[batch_index], as_tuple=False).flatten():
            members = membership[batch_index, group_index]
            if not bool((keep[batch_index] & members).any()):
                first = torch.nonzero(members & channel_mask[batch_index], as_tuple=False)[0, 0]
                keep[batch_index, first] = True
    return x * keep[:, :, None, None], keep


def apply_membership_group_dropout(
    group_membership: torch.Tensor,
    group_mask: torch.Tensor,
    probability: float,
    *,
    generator: torch.Generator | None = None,
) -> tuple[torch.Tensor, torch.Tensor]:
    """Drop complete anonymous groups without exposing group identity."""

    if not 0.0 <= probability < 1.0:
        raise ValueError("probability must be in [0, 1)")
    if probability == 0.0:
        return group_membership, group_mask
    keep = (
        torch.rand(group_mask.shape, device=group_mask.device, generator=generator)
        >= probability
    ) & group_mask
    for row in range(keep.shape[0]):
        if not bool(keep[row].any()):
            first = torch.nonzero(group_mask[row], as_tuple=False)[0, 0]
            keep[row, first] = True
    return group_membership & keep[:, :, None], keep
