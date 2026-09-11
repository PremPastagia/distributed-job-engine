#!/usr/bin/env node
// Renders the documents that quote measurements, substituting {{metric_key}} placeholders
// with values from results/verified_metrics.json:
//
//   docs/readme.template.md       -> README.md
//   docs/cv_pointers.template.md  -> CV_POINTERS_DISTRIBUTED_JOB_ENGINE.md
//   docs/final_report.template.md -> FINAL_PROJECT_REPORT.md
//
// This is the mechanism that makes an invented number impossible: a placeholder whose key
// is not a verified metric is an error, and scripts/gates/g17_claims.mjs re-renders both
// documents and recomputes the values from the raw samples.
import fs from 'node:fs';
import path from 'node:path';
import { ROOT } from './gates/_lib.mjs';

const METRICS = path.join(ROOT, 'results', 'verified_metrics.json');
const TABLE = path.join(ROOT, 'results', 'metrics_table.md');
const DOCUMENTS = [
  ['docs/readme.template.md', 'README.md'],
  ['docs/cv_pointers.template.md', 'CV_POINTERS_DISTRIBUTED_JOB_ENGINE.md'],
  ['docs/final_report.template.md', 'FINAL_PROJECT_REPORT.md'],
];

if (!fs.existsSync(METRICS)) {
  console.error(`missing ${path.relative(ROOT, METRICS)}`);
  console.error('run: node scripts/run_benchmarks.mjs && node scripts/extract_metrics.mjs');
  process.exit(1);
}
const metrics = JSON.parse(fs.readFileSync(METRICS, 'utf8'));

let failed = false;
for (const [templateRel, outputRel] of DOCUMENTS) {
  const templatePath = path.join(ROOT, templateRel);
  if (!fs.existsSync(templatePath)) {
    console.error(`missing ${templateRel}`);
    failed = true;
    continue;
  }
  const template = fs.readFileSync(templatePath, 'utf8');
  const unknown = new Set();
  let substitutions = 0;

  let rendered = template.replace(/\{\{([A-Za-z0-9_.]+)\}\}/g, (_, key) => {
    if (key === 'METRICS_TABLE') {
      return fs.existsSync(TABLE) ? fs.readFileSync(TABLE, 'utf8').trimEnd()
                                  : '(no metrics table)';
    }
    const m = metrics[key];
    if (!m) { unknown.add(key); return `{{MISSING:${key}}}`; }
    substitutions++;
    return String(m.value);
  });

  if (unknown.size > 0) {
    console.error(`${templateRel} references metrics that were never measured:`);
    for (const k of unknown) console.error(`  ${k}`);
    failed = true;
    continue;
  }
  rendered = rendered.replace(/<!-- RENDERED_AT -->/g, `<!-- rendered from ${templateRel} -->`);
  fs.writeFileSync(path.join(ROOT, outputRel), rendered);
  console.log(`rendered ${outputRel} from ${templateRel} with ${substitutions} measured value(s)`);
}

if (failed) {
  console.error('\nrun: node scripts/run_benchmarks.mjs && node scripts/extract_metrics.mjs');
  process.exit(1);
}
