# AOS_project

## Working Principles of Adaptive_cpu in a Nutshell

### 1) First round:
  - If available_cpu>0, next_quota=min(available_cpu,INITIAL_DEFAULT_QUOTA)
  - If available_cpu=0, the scheduling of the app is skipped for this round

### 2) In all rounds after the first:
 Consider delta= prev_quota-prev_cpu_usage >=0, slack as an amount of cpu proportional to the previous round quota and surplus as an amount of cpu proportional to delta.
  - If 0 < delta < ADMISSIBLE_DELTA, the same quota of the previous round is assigned
  - if delta>ADMISSIBLE_DELTA, next_quota=prev_quota-surplus
  - If delta==0, next_quota=prev_quota + min(slack, available_cpu)
