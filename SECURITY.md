# Security Policy

## Supported Versions

| Version | Supported          | Known Issues                                           |
| ------- | ------------------ | ------------------------------------------------------ |
| 1.x     | :warning:          | PTY stopping, af_inet sockets unstable, 300KB mem leak, unix sockets unstable |
| < 1.0   | :x:                | End of support                               |

## Known Issues

### Version 1.x
- **PTY Stopping Issue**: PTY (pseudo-terminal) processes may stop unexpectedly under certain conditions. Workaround: restart the terminal session.
- **AF_INET Sockets**: IPv4 socket operations are not fully stable. Some network operations may fail or hang.
- **AF_UNIX Sockets**: Some socket operations are not fully stable. Some sockets may hang on sending or receiving data.
- **Memory Leak**: Approximately 300KB memory leak detected during normal system operation.

## Reporting a Vulnerability

To report a security vulnerability in AscentOS:

1. **Do not** open a public GitHub issue
2. Email security concerns to the maintainers with:
   - Description of the vulnerability
   - Steps to reproduce (if applicable)
   - Affected version(s)
   - Suggested fix (if you have one)

We will acknowledge your report within 7 days and provide updates on progress every 2 weeks until resolved or declined.

## Security Expectations

AscentOS is an experimental operating system. Users should understand:
- This is not a production-ready OS
- Security patches may not be available for all issues
- Users are encouraged to report issues to help improve stability
- Kernel and core components may have unidentified security issues
