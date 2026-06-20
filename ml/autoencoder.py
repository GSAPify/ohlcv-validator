"""Autoencoder anomaly detector -- rung 3 of the ML ladder.

A small MLP autoencoder trained to reconstruct the feature vector. It learns the
*joint* structure of normal bars; an input that violates that structure (e.g. a
feature combination it never saw in training) reconstructs poorly, so per-row
reconstruction error is the anomaly score.

This is the rung that earns torch. It exists to beat rung 2 (the univariate
robust-z baseline) on exactly the anomalies the baseline structurally can't see:
correlation breaks, where every feature is marginally normal but the combination
isn't. See ml/test_autoencoder.py for that head-to-head, and ml/README.md for the
honesty caveat (real-world eval still needs live-captured data; the synthetic
anomaly here is constructed to be rule- and baseline-invisible to demonstrate the
mechanism, not to claim field performance).
"""

from __future__ import annotations

import numpy as np
import torch
from torch import nn


class _Net(nn.Module):
    def __init__(self, n_features: int, hidden: int, bottleneck: int):
        super().__init__()
        self.encoder = nn.Sequential(
            nn.Linear(n_features, hidden),
            nn.ReLU(),
            nn.Linear(hidden, bottleneck),
            nn.ReLU(),
        )
        self.decoder = nn.Sequential(
            nn.Linear(bottleneck, hidden),
            nn.ReLU(),
            nn.Linear(hidden, n_features),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.decoder(self.encoder(x))


class Autoencoder:
    """Reconstruction-error anomaly detector. Fit on normal data, score anything.

    Standardization (mean/std) is learned on the fit data so the model and the
    scorer see the same scale -- features here span very different magnitudes
    (log-return ~1e-3 vs trade_count ~1e2), and an un-normalized MSE would be
    dominated by the largest-magnitude feature.
    """

    def __init__(
        self,
        hidden: int = 16,
        bottleneck: int = 4,
        epochs: int = 400,
        lr: float = 1e-3,
        seed: int = 0,
    ):
        self.hidden = hidden
        self.bottleneck = bottleneck
        self.epochs = epochs
        self.lr = lr
        self.seed = seed
        self._net: _Net | None = None
        self._mean: np.ndarray | None = None
        self._std: np.ndarray | None = None

    def _standardize(self, x: np.ndarray) -> np.ndarray:
        return (x - self._mean) / self._std

    def fit(self, x: np.ndarray) -> "Autoencoder":
        torch.manual_seed(self.seed)
        np.random.seed(self.seed)

        self._mean = x.mean(axis=0)
        # Guard zero-variance features so standardization can't divide by zero.
        self._std = x.std(axis=0)
        self._std[self._std == 0] = 1.0

        xs = torch.tensor(self._standardize(x), dtype=torch.float32)
        self._net = _Net(x.shape[1], self.hidden, self.bottleneck)
        opt = torch.optim.Adam(self._net.parameters(), lr=self.lr)
        loss_fn = nn.MSELoss()

        self._net.train()
        for _ in range(self.epochs):
            opt.zero_grad()
            loss = loss_fn(self._net(xs), xs)
            loss.backward()
            opt.step()
        return self

    def score(self, x: np.ndarray) -> np.ndarray:
        """Per-row reconstruction MSE on standardized features -> anomaly score."""
        if self._net is None:
            raise RuntimeError("Autoencoder.score called before fit")
        self._net.eval()
        xs = torch.tensor(self._standardize(x), dtype=torch.float32)
        with torch.no_grad():
            recon = self._net(xs)
            err = ((recon - xs) ** 2).mean(dim=1)
        return err.numpy()
