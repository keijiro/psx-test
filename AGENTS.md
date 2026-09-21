Development Guidelines
======================

Where the reasoning lives
-------------------------

Keep each explanation where a future reader will naturally look for it, and do not
duplicate the same reasoning in several places.

- **A code comment owns why that code is shaped the way it is.** Explain decisions,
  constraints, rejected alternatives, and measured constants beside the code they
  govern. If a change makes a comment inaccurate, update the comment in the same edit.
- **Documentation owns reasoning that spans files or has no natural home in one file.**
  Use it for specifications, cross-cutting rules, and architectural decisions. Do not
  add documentation that merely paraphrases a file's comments.
- **A commit message owns why the change happened at all.** Record the state before the
  change, relevant external context, and decisions or experiments that left no code or
  comment behind.

Do not repeat a file's comment in documentation, move a local explanation away from the
code it governs, or copy reasoning into a commit body when the same commit already puts
that reasoning beside the code.

Coding and comments
-------------------

Write documentation and code comments in English.

Follow the surrounding code. Preserve its conventions, naming, structure, and level of
abstraction unless the change deliberately improves them.

Comments are part of the implementation. Match the voice and precision of the existing
prose rather than flattening it into a summary. Prefer comments that explain why a
choice exists over comments that restate what the code visibly does.

Keep changes coherent: code, comments, and any affected documentation should agree in
the same edit.

Commit messages
---------------

Open with an imperative subject that says what changed. Do not end it with a full stop.
Keep it under 72 characters and aim for about 50.

After the subject, add a blank line and explain the reason for the change: why the code
is shaped this way, what was tried, or what was measured. Use as many sentences and
paragraphs as the argument requires, wrapping body text at 72 columns.

Do not add length for its own sake. The message should be proportional to the diff, not
to the effort spent discovering the solution. If the detailed reasoning is already in
a comment added by the same commit, keep the body to a concise account of what changed
and any context that cannot be recovered from the resulting tree.
