# Security Policy

- The latest version receives security support for one year following its release. If a newer version is released before this period ends, the previous version continues to receive security updates until its original end date.
- Each release uses a unique version number following semantic versioning (X.Y.Z):
  - X: Major releases with thorough vulnerability testing and new features. X.0.0 releases are candidates for LTS designation (e.g., 2.0.0).
  - Y: Feature updates (e.g., 2.5.0 adds features to 2.0.0).
  - Z: Security patches (e.g., 2.5.1 patches 2.5.0). Changes in Z are generally backwards-compatible but not guaranteed.
  - Note: Version numbers listed are examples and may not correspond to actual releases.
- Long-Term Support (LTS) releases receive security patches for five years from their release date, excluding platform migration support or new features.
- Security patches are issued on an as-needed basis in response to identified vulnerabilities.

## Supported Versions

| Version | Support Status     |
| ------- | ------------------ |
| 2.5.0   | [YES] Security updates until December 10th, 2026 or until a newer version is released, whichever period is longer. |
| 2.0.0 LTS | [YES] Security updates until December 5th, 2030 |
| < 2.0.0  | [NO] Not supported |

## Reporting a Vulnerability

Use the Security tab at the top of this repository to submit a private vulnerability report. 
If the vulnerability is legitimate and reproducible, a fix will be implemented as soon as possible.

---

# Policy for AI usage in Vulnerability Reports
Fraudulent or AI-generated reports will result in the reporter being banned. Please refer to the policy below to understand acceptable use of AI.

## Acceptable Use of AI or LLM
Reporters must disclose all AI usage in their submission. AI is acceptable for the following purposes:

- Rephrasing or translating manually discovered vulnerabilities for clarity, particularly for non-native English speakers or individuals with disabilities.
- Generating report summaries, provided the content has been manually verified by the reporter.
- Improving readability of proof-of-concept code by renaming variables, functions, or methods, provided all changes have been manually verified.
- Rewriting comments or documentation within proof-of-concept code for clarity, provided all changes have been manually verified.

## Unacceptable Use of AI or LLM
The following uses will result in immediate rejection and may lead to a ban:

- Generating vulnerability reports without manual discovery, testing, or analysis.
- Submitting theoretical or impractical scenarios that have no real-world applicability to this codebase.
- Reporting issues unrelated to the codebase itself (e.g., claiming that malware disguises itself as conhost.exe is not a vulnerability in this project).
- Using AI to fabricate technical details, exploit scenarios, or impact assessments.
