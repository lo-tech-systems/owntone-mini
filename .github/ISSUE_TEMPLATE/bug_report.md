---
name: Bug or problem report
about: Use this if owntone-mini isn't working the way it should
title: ''
labels: ''
assignees: ''

---

Please try to provide the following:

- steps to reproduce and/or logging of the error
- version of owntone-mini
- platform

Steps to reproduce will greatly improve the chance of getting it fixed. If it is not possible then set the log level to debug and try to get some logging of when the error happens. You can set it with the `loglevel` key in the settings file (owntone-settings.json), by starting the daemon with `-d 5`, or at runtime with `PUT /api/settings/misc/loglevel`. Don't cut and paste lengthy log outtakes here on github. Instead, attach the log file, and remove parts of the log file that aren't relevant.
