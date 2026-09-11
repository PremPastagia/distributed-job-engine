// G19: every required deliverable document exists, is substantial, and contains no
// unfilled placeholder.
import fs from 'node:fs';
import path from 'node:path';
import { ROOT, check, checkEq, finish } from './_lib.mjs';

const required = [
  ['README.md', 2000],
  ['BENCHMARKS.md', 1500],
  ['TESTING.md', 2000],
  ['TEST_RESULTS.md', 1500],
  ['FAILURE_ANALYSIS.md', 2000],
  ['REPRODUCIBILITY.md', 1500],
  ['FINAL_PROJECT_REPORT.md', 3000],
  ['INTERVIEW_PREPARATION.md', 3000],
  ['CV_POINTERS_DISTRIBUTED_JOB_ENGINE.md', 3000],
  ['docs/REQUIREMENTS.md', 2000],
  ['docs/ARCHITECTURE.md', 3000],
  ['docs/DESIGN_DECISIONS.md', 3000],
  ['docs/ROADMAP.md', 800],
];

const placeholders = [/\bTODO\b/, /\bTBD\b/, /\bFIXME\b/, /\bXXX\b/, /\bLorem ipsum\b/i,
                      /\{\{[A-Za-z0-9_]+\}\}/, /<insert /i, /\[fill in\]/i, /\bN\/A yet\b/i];

// Words that would over-claim unless the evidence justifies them. The project deliberately
// avoids them; this keeps that discipline enforced rather than remembered.
const overclaims = [/production[- ]grade/i, /production[- ]ready/i, /highly scalable/i,
                    /infinitely scalable/i, /fault[- ]tolerant/i, /\bzero[- ]downtime\b/i,
                    /\bunlimited\b/i, /\bbulletproof\b/i, /\bblazing\b/i];

const summary = [];
let totalBytes = 0;
for (const [rel, minSize] of required) {
  const p = path.join(ROOT, rel);
  if (!fs.existsSync(p)) { check(false, `${rel} is missing`); continue; }
  const text = fs.readFileSync(p, 'utf8');
  totalBytes += text.length;
  check(text.length >= minSize,
        `${rel} is only ${text.length} bytes, below the ${minSize} expected for a real document`);
  // Strip inline code spans and fenced blocks first: these documents legitimately discuss
  // the templating mechanism, and `{{metric_key}}` written as code is prose about a
  // placeholder, not an unfilled one.
  const prose = text.replace(/```[\s\S]*?```/g, ' ').replace(/`[^`\n]*`/g, ' ');
  for (const re of placeholders) {
    const m = re.exec(prose);
    check(m === null, `${rel} contains an unfilled placeholder: ${m ? m[0] : ''}`);
  }
  // An over-claiming phrase is allowed only where the document explicitly states it does
  // NOT apply - either in prose ("I do not describe this as fault tolerant") or as a row of
  // an evidence table whose verdict column says **No**. Every occurrence is checked, not
  // just the first.
  const lines = text.split('\n');
  for (const re of overclaims) {
    for (let i = 0; i < lines.length; i++) {
      const line = lines[i];
      const m = re.exec(line);
      if (!m) continue;
      const rowVerdictIsNo = /^\s*\|/.test(line) && /\*\*No\*\*/.test(line);
      const context = lines.slice(Math.max(0, i - 2), i + 3).join(' ');
      const disclaimedInProse =
          /\bnot\b|\bavoid|\bdo not\b|\bnever\b|\bwithout\b|\bcannot\b|\bunless\b/i
              .test(context);
      check(rowVerdictIsNo || disclaimedInProse,
            `${rel}:${i + 1} uses "${m[0]}" without disclaiming it: ${line.trim().slice(0, 160)}`);
    }
  }
  summary.push(`${rel}: ${text.length} bytes`);
}

// Cross-references must resolve: a document that points at a file that does not exist is
// a broken deliverable.
let brokenLinks = 0;
for (const [rel] of required) {
  const p = path.join(ROOT, rel);
  if (!fs.existsSync(p)) continue;
  const text = fs.readFileSync(p, 'utf8');
  for (const m of text.matchAll(/\]\(([^)#:]+\.(?:md|csv|png|sh|mjs|py|cpp|hpp))\)/g)) {
    const target = path.resolve(path.dirname(p), m[1]);
    if (!fs.existsSync(target)) {
      console.error(`  ${rel} links to ${m[1]}, which does not exist`);
      brokenLinks++;
    }
  }
}
checkEq(brokenLinks, 0, `${brokenLinks} broken cross-reference(s) between documents`);

// The limitations must be stated, not implied.
const report = fs.existsSync(path.join(ROOT, 'FINAL_PROJECT_REPORT.md'))
    ? fs.readFileSync(path.join(ROOT, 'FINAL_PROJECT_REPORT.md'), 'utf8') : '';
for (const topic of ['at-least-once', 'single point of failure', 'cooperative',
                     'Docker', 'not tested']) {
  check(report.toLowerCase().includes(topic.toLowerCase()),
        `FINAL_PROJECT_REPORT.md does not discuss "${topic}"`);
}

finish('G19', [
  `${required.length} required documents present, ${totalBytes} bytes total`,
  `no placeholders, no unqualified over-claims, no broken cross-references`,
]);
