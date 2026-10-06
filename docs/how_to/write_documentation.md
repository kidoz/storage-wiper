# Write documentation

Use this guide when adding a page or updating documentation for a code change.
The documentation is ordinary Markdown in `docs/`; it does not require a site
generator to read in a checkout or on GitHub.

## Choose the reader's need

Choose one primary purpose for each page using the
[Diátaxis framework](https://diataxis.fr/start-here/):

| Reader's need | Placement | Shape |
| --- | --- | --- |
| Learn by doing | `docs/tutorials/` | A guided exercise with prerequisites, fixed steps, and observable results |
| Complete a known task | `docs/how_to/` | A goal, practical steps, relevant branches, and a completion check |
| Look up exact information | `docs/reference/` | Structured descriptions of options, schemas, defaults, or contracts |
| Understand why | `docs/explanation/` | Context, reasoning, relationships, and tradeoffs |

Link to other forms when the reader needs them. For example, a wipe how-to can
link to the algorithm reference and sanitization explanation without reproducing
both inside its steps.

## Write and connect the page

1. Use a descriptive snake_case filename and one `#` title.
2. State the outcome or subject immediately. For procedures, state prerequisites
   before commands.
3. Use relative links to related pages and source files. Add the page to its
   section's `README.md`; add a route on the documentation home when needed.
4. End with links to the section index and documentation home.
5. Keep the root project README as an overview and entry point.

Use short headings, concrete names, and fenced examples with a language such
as `bash`, `json`, or `ini`. Include the working directory and mark illustrative
output or placeholders. Keep tutorials on temporary files, simulated fixtures,
or read-only operations. Put destructive templates in task guides with the
target-selection steps they depend on.

## Verify against the implementation

Check the source of the behavior you describe:

| Subject | Source |
| --- | --- |
| Version, dependencies, targets, and installation paths | [meson.build](../../meson.build), [meson_options.txt](../../meson_options.txt) |
| Convenience commands | [justfile](../../justfile) |
| CLI flags and output | [CliApplication.cpp](../../src/cli/CliApplication.cpp) |
| Algorithm IDs and category labels | [WipeTypes.hpp](../../src/models/WipeTypes.hpp) |
| D-Bus types and authorization | [DBusSignatures.hpp](../../src/services/DBusSignatures.hpp), [helper main](../../src/helper/main.cpp), [polkit policy](../../data/su.kidoz.storage_wiper.policy) |
| Settings, logs, and certificates | [AppSettings.cpp](../../src/util/AppSettings.cpp), [Logger.hpp](../../src/util/Logger.hpp), [WipeCertificate.cpp](../../src/util/WipeCertificate.cpp) |

Verify commands with `--help`, dry runs, or simulated tests when possible. Check
destructive examples against the implementation without executing them on a
real device. Avoid recording a fixed test count or copying version strings
across pages when a command or source link supplies the current value.

Cite primary sources for standards or external claims. Keep the application's
reported categories distinct from independently established device behavior or
compliance.

## Review the result

- Open the page in a Markdown renderer and check headings, tables, fences, and
  image descriptions.
- Follow every local link, including heading anchors, and confirm that the
  page is reachable from its section index.
- Check shell snippets for syntax and confirm option names, test filters, paths,
  and expected results against the current code.
- Review the diff for duplicated instructions and factual drift in related pages.

[How-to guides](README.md) · [Documentation home](../README.md)
