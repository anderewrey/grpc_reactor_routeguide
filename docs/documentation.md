# Writing documentation

This document sets the direction for how documentation in this repository gets written, so the same guidance
doesn't need repeating every time someone writes a new doc or edits an existing one.

## Borrowed direction, not rules

This project looks at established documentation style guides for direction, not as rules to apply mechanically.
Each contributes something specific:

- [Google Developer Documentation Style Guide](https://developers.google.com/style): sentence case, clarity,
  writing for scanning first.
- [Microsoft Writing Style Guide](https://learn.microsoft.com/en-us/style-guide/welcome/): a warm, direct voice,
  and a caution against leaning on dashes, colons, or parentheses as a crutch.
- [GitLab Documentation Style Guide](https://docs.gitlab.com/development/documentation/styleguide/): sentence
  case justified by accessibility (screen readers read capitals letter by letter), and clarity that survives
  translation.

## What this project requires

**Capitalize only the first word and proper nouns in headings.** Technical names (gRPC, EventLoop, C++20, Active
Object) keep their own capitalization; everything else in a heading stays lowercase.

**Each sentence carries one complete idea, closed with a period.** Introduce a list or an explanation with a
colon. Add a short aside with parentheses or a comma. Give a second clause its own sentence. When a sentence
lists parallel facts, use a table or a bulleted list instead.

For example, instead of:

> The Active Object pattern removes this problem instead of managing it, the Proxy returns immediately, and the
> Servant runs later on the application thread.

use a table, since this is really two parallel facts about two components:

| Component | Behavior |
| --------- | ---------- |
| Proxy | Returns immediately |
| Servant | Runs later, on the application thread |

**Convey emphasis and warnings with words, not symbols.** No emoji or decorative Unicode symbols in headings or
text. GitHub's alert blocks (`[!NOTE]`, `[!IMPORTANT]`, `[!WARNING]`, and so on) are the exception: use them for
callouts, since this project is hosted and read on GitHub, and its Markdown viewer renders them natively.

**Confirm a technical claim against the actual code before writing it down.** A statement about what the code
does, requires, or depends on needs a file and line behind it, not a plausible-sounding guess.

**Write for the next contributor.** Don't write sections addressed to a specific kind of contributor; the
guidance in this document applies to whoever is writing next.

## Deciding where content goes

[Diátaxis](https://diataxis.fr/) identifies four kinds of documentation, each answering a different need:

- **Tutorial**: a guided lesson for someone learning by doing.
- **How-to guide**: steps to reach a specific, practical goal.
- **Reference**: technical facts, structured to mirror what they describe.
- **Explanation**: the reasoning and context behind a design, answering "why."

This project's existing docs map onto three of the four:

| Document | Kind | Content |
| ---------- | ------ | --------- |
| [developing.md](/docs/developing.md), [testing.md](/docs/testing.md) | How-to guide | Build, run, and test steps |
| [reactor_client.md](/applications/reactor/reactor_client.md) | Reference | Class-by-class API, mirroring the actual reactor hierarchy |
| [architecture.md](/docs/architecture.md) | Explanation | Design rationale: why this pattern, why these choices |

Before adding to a document, decide which of these four kinds the new content is, and put it in the document
that matches. Reasoning and trade-offs belong in `architecture.md`, not `reactor_client.md`. Class-by-class
mechanics belong in `reactor_client.md`, not `architecture.md`. Neither should carry step-by-step build commands;
those belong in `developing.md`.
