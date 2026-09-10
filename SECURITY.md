# Security Policy

## Intended use

Remote Phone Monitor is intended only for devices you own or are explicitly
authorized to administer. Screen captures can contain passwords, messages,
financial information, and other sensitive data. Obtain informed consent from
every affected user and follow applicable laws.

Do not expose the server directly to the Internet without HTTPS, a strong
administrator password, unique device tokens, firewall rules, and regular
updates. Rotate any credential that may have been disclosed.

## Reporting a vulnerability

Please use GitHub's private vulnerability reporting feature for this
repository. Do not include real screenshots, access tokens, server addresses,
device identifiers, or other personal data in a public issue.

## Deployment checklist

- Replace every `CHANGE_ME_...` placeholder with an independently generated
  random value.
- Keep `server.json`, `tokens.json`, databases, screenshots, logs, queues, and
  packaged binaries out of version control.
- Put the FastAPI service behind an HTTPS reverse proxy.
- Restrict network access and run the service as an unprivileged user.
- Set short data-retention and storage limits appropriate for your use case.
