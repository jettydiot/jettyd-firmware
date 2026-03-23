#!/usr/bin/env node
/**
 * Firmware Safety Review Agent
 *
 * Analyses ESP32/ESP-IDF C firmware code for embedded-specific failure modes
 * that LLM-generated code commonly gets wrong.
 *
 * Usage:
 *   node safety-review.js apps/firmware/drivers/relay/relay.c
 *   node safety-review.js apps/firmware/drivers/*.c
 *   node safety-review.js --all   # scans all .c files in apps/firmware/
 */

import Anthropic from "@anthropic-ai/sdk";
import { readFileSync } from "fs";
import path from "path";
import { globSync } from "glob";

const client = new Anthropic({ apiKey: process.env.ANTHROPIC_API_KEY });

const FIRMWARE_ROOT = path.resolve(
  path.dirname(new URL(import.meta.url).pathname),
  "..",
);

const SAFETY_PROMPT = `You are a firmware safety reviewer specialising in ESP32/ESP-IDF embedded C code.
You review code for specific embedded failure modes, in priority order.

### CRITICAL (block merge)
1. **Missing \`volatile\` on shared variables** — Any variable read by an ISR and written by a task (or vice versa) must be \`volatile\`. Missing volatile causes the compiler to cache the value in a register, silently reading stale data.
2. **Non-atomic read-modify-write on shared state** — \`x = x + 1\` on a shared variable is 3 instructions. An ISR can fire between the read and the write. Must use atomic operations or critical sections (\`portENTER_CRITICAL\`/\`portEXIT_CRITICAL\`).
3. **Blocking calls in ISR context** — \`vTaskDelay\`, \`xQueueReceive\` with timeout, \`printf\`, \`malloc\` are all illegal in ISR context. ISRs must be short and non-blocking. Use \`xQueueSendFromISR\` / \`FromISR\` variants.
4. **Dynamic allocation after init** — \`malloc\`/\`calloc\`/\`realloc\` in the main loop or task functions is dangerous on embedded. Memory fragmentation causes hard-to-reproduce crashes. All allocation should happen at init time.
5. **Stack overflow risk** — Large local arrays on the stack (e.g., \`char buf[4096]\` inside a function) will overflow the ESP32 task stack. Flag any local array > 512 bytes.
6. **Missing max_on_duration enforcement** — For any switchable driver (relay, solenoid, PWM), the spec requires \`max_on_duration\` to be enforced by a hardware timer or watchdog. If a driver has \`switch_on\` but no auto-off timer, flag it.

### WARNING (flag for review)
7. **GPIO not configured before use** — GPIO must be configured (\`gpio_config()\` or \`gpio_set_direction()\`) before \`gpio_set_level()\` or \`gpio_get_level()\`. If a function calls gpio_set/get before config, flag it.
8. **I2C/SPI timeout not handled** — I2C reads (\`i2c_master_read_from_device\`) can hang if the device is not present. The return value must be checked; a timeout must be set.
9. **ADC reading without attenuation check** — ESP32 ADC has known non-linearity above 3.0V without proper attenuation. If \`adc1_get_raw\` is called without preceding attenuation config, flag it.
10. **Delay in main init sequence** — Long blocking delays (\`vTaskDelay(pdMS_TO_TICKS(2000))\`) in the boot/init path will delay the watchdog kick and can cause unexpected resets.
11. **No error handling on ESP-IDF calls** — Most ESP-IDF functions return \`esp_err_t\`. If the return value is discarded (not checked, not logged), flag it.
12. **Missing null check on driver registry** — If \`jettyd_driver_find()\` or \`jettyd_driver_get()\` result is used without null check, flag it.

### INFO (good to know)
13. **Hardcoded GPIO pins** — GPIO numbers hardcoded in source rather than coming from config struct. This makes device variants impossible without code changes.
14. **Magic numbers** — Unexplained numeric literals (not 0, 1, or obvious constants) that should be named \`#define\`s.
15. **No self-test implementation** — If the driver struct declares a \`self_test\` function pointer but it is NULL or unimplemented, note it.

Review the following C file and respond with ONLY a structured report in this exact format.
Do NOT include any text before or after the report. Every issue must reference a specific line number.

If there are issues:
\`\`\`
CRITICAL ISSUES (<count>):
  [C<n>] Line <num>: <description>

WARNINGS (<count>):
  [W<n>] Line <num>: <description>

INFO (<count>):
  [I<n>] Line <num>: <description>

Summary: <critical_count> CRITICAL, <warning_count> WARNING, <info_count> INFO
Result: FAILED
\`\`\`

If a category has 0 issues, still include the header with (0) and leave it empty.
If there are zero issues across all categories:
\`\`\`
Summary: 0 CRITICAL, 0 WARNING, 0 INFO
Result: PASSED
\`\`\`

IMPORTANT: Only flag issues you are confident about. Do not speculate. Reference exact line numbers and variable/function names.`;

async function reviewFile(filePath) {
  const absPath = path.resolve(filePath);
  const relPath = path.relative(process.cwd(), absPath);
  const source = readFileSync(absPath, "utf-8");
  const fileName = path.basename(absPath);

  console.log(`\n=== Safety Review: ${fileName} ===\n`);

  const message = await client.messages.create({
    model: "claude-opus-4-20250901",
    max_tokens: 4096,
    messages: [
      {
        role: "user",
        content: `${SAFETY_PROMPT}\n\nFile: ${relPath}\n\n\`\`\`c\n${source}\n\`\`\``,
      },
    ],
  });

  const report = message.content[0].text.trim();
  console.log(report);
  console.log("");

  // Parse result
  const summaryMatch = report.match(
    /(\d+)\s+CRITICAL,\s*(\d+)\s+WARNING,\s*(\d+)\s+INFO/,
  );
  const criticalCount = summaryMatch ? parseInt(summaryMatch[1], 10) : 0;
  const warningCount = summaryMatch ? parseInt(summaryMatch[2], 10) : 0;
  const infoCount = summaryMatch ? parseInt(summaryMatch[3], 10) : 0;

  if (criticalCount > 0) {
    console.log(
      `Result: \u274C FAILED \u2014 merge blocked until CRITICAL issues resolved.\n`,
    );
  } else {
    console.log(`Result: \u2705 PASSED\n`);
  }

  return { file: relPath, criticalCount, warningCount, infoCount };
}

async function main() {
  const args = process.argv.slice(2);

  if (args.length === 0) {
    console.error(
      "Usage: node safety-review.js <file.c ...> | --all",
    );
    process.exit(1);
  }

  let files;
  if (args.includes("--all")) {
    files = globSync("**/*.c", { cwd: FIRMWARE_ROOT, absolute: true });
    // Exclude build artifacts and test files
    files = files.filter(
      (f) => !f.includes("/build/") && !f.includes("/test/"),
    );
    files.sort();
  } else {
    files = args.map((f) => path.resolve(f));
  }

  if (files.length === 0) {
    console.error("No .c files found.");
    process.exit(1);
  }

  console.log(`Reviewing ${files.length} file(s)...\n`);

  let totalCritical = 0;
  let totalWarning = 0;
  let totalInfo = 0;

  for (const file of files) {
    const result = await reviewFile(file);
    totalCritical += result.criticalCount;
    totalWarning += result.warningCount;
    totalInfo += result.infoCount;
  }

  if (files.length > 1) {
    console.log("=".repeat(50));
    console.log(
      `TOTAL: ${totalCritical} CRITICAL, ${totalWarning} WARNING, ${totalInfo} INFO`,
    );
    if (totalCritical > 0) {
      console.log(
        `\u274C FAILED \u2014 ${totalCritical} critical issue(s) must be resolved.`,
      );
    } else {
      console.log(`\u2705 ALL PASSED`);
    }
  }

  process.exit(totalCritical > 0 ? 1 : 0);
}

main().catch((err) => {
  console.error("Fatal error:", err.message);
  process.exit(2);
});
