#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 - "${ROOT_DIR}" <<'PY'
from __future__ import annotations

import pathlib
import sys

root = pathlib.Path(sys.argv[1])

documents = {
    "standard_home_v1/Documents/project-proposal.pdf": """Title: BetterSpotlight Expansion Proposal
Author: Product Strategy

Executive summary
This proposal outlines a phased rollout for semantic relevance improvements.
The focus is reliability, maintainability, and measurable user outcomes.

Section 1: Problem Statement
Users can find exact filenames quickly, but conceptual search lags.
Current ranking fails on intent-heavy queries.

Section 2: Scope and timeline
- Phase A: corpus and fixture setup
- Phase B: merge tuning and offline eval
- Phase C: rollout and monitor
Estimated timeline is eight weeks.

Section 3: Resourcing
Engineering: 3 FTE
Design: 1 FTE
QA: 1 FTE

Section 4: budget allocation
Budget allocation prioritizes infra test coverage and onboarding analytics.
Contingency reserved for extraction edge cases.
""",
    "standard_home_v1/Documents/quarterly-review-q4.pdf": """Quarterly Business Review Q4 2025
Prepared by: Operations

Highlights
- On-time roadmap delivery improved from 78% to 91%.
- Uptime reached 99.95%.
- Support ticket backlog dropped by 32%.

KPI Deep Dive
The report tracks performance metrics across search latency and quality.
Team execution met quarterly targets for indexing stability.
Revenue analysis showed sustained revenue growth in enterprise accounts.

Risks
- Model drift for semantic ranking on rare intents.
- Single maintainer risk in extraction subsystem.

Next Steps
- Add targeted tests for edge-case query handling.
- Expand fixture diversity with legal and research documents.
- Review scoring weights with product analytics.
""",
    "standard_home_v1/Documents/research-paper-ml.pdf": """Paper: Efficient Retrieval Augmentation for Desktop Search
Authors: Internal Research Group

Abstract
We evaluate retrieval quality under constrained local compute.
The study compares lexical retrieval, semantic retrieval, and hybrid scoring.

Method
A controlled corpus with deterministic content is indexed in SQLite FTS5.
Semantic vectors are generated with quantized embedding models.

Results
Hybrid retrieval outperforms lexical-only systems on conceptual tasks.
Latency remains acceptable with bounded candidate expansion.

Conclusion
High-precision lexical matching should be preserved.
Semantic scoring should augment, not replace, deterministic ranking.
# note line 1
""",
    "standard_home_v1/Documents/tax-return-2025.pdf": """Tax Return Summary 2025
Filing Status: Single
Adjusted Gross Income: 143200
Federal Tax Withheld: 26400
State Tax Withheld: 8100

Deductions
- Mortgage interest
- Charitable contributions
- Student loan interest

Attachments
- W-2 forms
- 1099-INT
- Brokerage summary

Notes
Prepared by accounting software export.
Retain for seven years.
""",
    "standard_home_v1/Downloads/invoice-january-2026.pdf": """Invoice Number: INV-2026-0017
Billing Date: 2026-01-31
Due Date: 2026-02-15

Vendor: CloudNode Hosting
Customer: BetterSpotlight LLC

Line Items
- Compute instances: 1200.00
- Managed database: 420.00
- Object storage: 85.00
- Support plan: 199.00

Total Due: 1904.00 USD
# note line 1
# note line 2
# note line 3
# note line 4
# note line 5
""",
    "standard_home_v1/Downloads/software-license-agreement.pdf": """Software License Agreement
Version: 3.2

1. Grant of License
The vendor grants a non-exclusive, non-transferable license.

2. Restrictions
No reverse engineering except where permitted by law.
No resale without written authorization.

3. Warranty Disclaimer
Software provided "as is" without implied warranties.

4. Limitation of Liability
Liability is limited to fees paid in preceding 12 months.
# note line 1
# note line 2
# note line 3
# note line 4
""",
}


def escape_pdf_literal(text: str) -> bytes:
    return (
        text.replace("\\", "\\\\")
        .replace("(", "\\(")
        .replace(")", "\\)")
        .encode("utf-8")
    )


def build_pdf(text: str) -> bytes:
    lines = text.strip("\n").splitlines()
    y = 760
    commands: list[bytes] = [b"BT", b"/F1 12 Tf"]
    first = True
    for line in lines:
        if first:
            commands.append(f"72 {y} Td".encode("ascii"))
            first = False
        else:
            commands.append(b"0 -16 Td")
        commands.append(b"(" + escape_pdf_literal(line) + b") Tj")
    commands.append(b"ET")
    content = b"\n".join(commands) + b"\n"

    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        (
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>"
        ),
        b"<< /Length " + str(len(content)).encode("ascii") + b" >>\nstream\n" + content + b"endstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]

    pdf = bytearray(b"%PDF-1.4\n")
    offsets: list[int] = []
    for index, obj in enumerate(objects, start=1):
        offsets.append(len(pdf))
        pdf.extend(f"{index} 0 obj\n".encode("ascii"))
        pdf.extend(obj)
        pdf.extend(b"\nendobj\n")

    xref_offset = len(pdf)
    pdf.extend(f"xref\n0 {len(objects) + 1}\n".encode("ascii"))
    pdf.extend(b"0000000000 65535 f \n")
    for offset in offsets:
        pdf.extend(f"{offset:010d} 00000 n \n".encode("ascii"))
    pdf.extend(
        b"trailer\n<< /Size "
        + str(len(objects) + 1).encode("ascii")
        + b" /Root 1 0 R >>\n"
    )
    pdf.extend(b"startxref\n")
    pdf.extend(str(xref_offset).encode("ascii"))
    pdf.extend(b"\n%%EOF\n")
    return bytes(pdf)


for relative_path, text in documents.items():
    target = root / relative_path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(build_pdf(text))
    print(f"wrote {target}")
PY
