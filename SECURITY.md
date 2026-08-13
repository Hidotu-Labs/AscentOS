# Security Policy

## Supported Versions

| Version | Supported          | Known Issues                                           |
| ------- | ------------------ | ------------------------------------------------------ |
| 2.x     | :warning:          | No issues                                         |
| 1.x     | :warning:          | PTY stopping, 300KB mem leak |
| < 1.0   | :x:                | End of support                                         |

## Known Issues

### Version 2.x
- **No issues**: Currently no known issues other than speed


### Version 1.x
- **PTY Stopping Issue**: PTY (pseudo-terminal) processes may stop unexpectedly under certain conditions. Workaround: restart the terminal session.
- **Memory Leak**: Approximately 300KB memory leak detected during normal system operation.

## Reporting a Vulnerability

To report a security vulnerability in AvoryOS:

1. **Do not** open a public GitHub issue
2. Email security concerns to the maintainers with:
   - Description of the vulnerability
   - Steps to reproduce (if applicable)
   - Affected version(s)
   - Suggested fix (if you have one)

We will acknowledge your report within 7 days and provide updates on progress every 2 weeks until resolved or declined.

## Security Expectations

AvoryOS is an experimental operating system. Users should understand:
- This is not a production-ready OS
- Security patches may not be available for all issues
- Users are encouraged to report issues to help improve stability
- Kernel and core components may have unidentified security issues
