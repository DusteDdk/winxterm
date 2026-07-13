#ifndef WINXTERM_JOB_PLAN_H
#define WINXTERM_JOB_PLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WINXTERM_JOB_PLAN_MAX_STAGES 64u
#define WINXTERM_JOB_PLAN_MAX_ARGUMENTS 1024u
#define WINXTERM_JOB_PLAN_MAX_STRING_BYTES (64u * 1024u)
#define WINXTERM_JOB_PLAN_MAX_ENVIRONMENT 4096u

typedef enum WinxtermJobPlanEndpoint {
    WINXTERM_JOB_PLAN_ENDPOINT_TERMINAL = 0,
    WINXTERM_JOB_PLAN_ENDPOINT_PIPE = 1,
    WINXTERM_JOB_PLAN_ENDPOINT_FILE = 2,
    WINXTERM_JOB_PLAN_ENDPOINT_CONNECTABLE = 3
} WinxtermJobPlanEndpoint;

#define WINXTERM_JOB_PLAN_FLAG_BACKGROUND 0x00000001u
#define WINXTERM_JOB_PLAN_FLAG_CONNECTABLE_STDIN 0x00000002u
#define WINXTERM_JOB_STAGE_FLAG_ISOLATED_BUILTIN 0x00000001u
#define WINXTERM_JOB_STAGE_FLAG_APPEND 0x00000002u
#define WINXTERM_JOB_STAGE_FLAG_TEE 0x00000004u

typedef struct WinxtermJobPlanStage {
    char **arguments;
    size_t argument_count;
    WinxtermJobPlanEndpoint stdin_endpoint;
    WinxtermJobPlanEndpoint stdout_endpoint;
    WinxtermJobPlanEndpoint stderr_endpoint;
    uint32_t flags;
    char *path;
} WinxtermJobPlanStage;

typedef struct WinxtermJobExecutionPlan {
    char *cwd;
    char **environment;
    size_t environment_count;
    WinxtermJobPlanStage *stages;
    size_t stage_count;
    uint32_t flags;
} WinxtermJobExecutionPlan;

bool winxterm_job_plan_encode(const WinxtermJobExecutionPlan *plan,
                             uint8_t **payload, uint32_t *payload_length);
bool winxterm_job_plan_decode(const uint8_t *payload, size_t payload_length,
                             WinxtermJobExecutionPlan *plan);
void winxterm_job_plan_dispose(WinxtermJobExecutionPlan *plan);

#endif
