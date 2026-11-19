package rollup

import (
	"context"
	"fmt"
	"log"
	"time"

	"miacstats/internal/config"
)

// Backend defines database operations needed by the rollup runner.
type Backend interface {
	GetRollupState(ctx context.Context) (watermark int64, lastRunAt int64, minRawTs int64, err error)
	RollupWindow(ctx context.Context, fromUTC, toUTC int64, dryRun bool) error
	SetRollupLastRunAt(ctx context.Context, ts int64) error
}

// Runner coordinates background or on-demand rollup execution.
type Runner struct {
	cfg     config.Config
	backend Backend
	nowFn   func() time.Time
}

// NewRunner creates a new Runner instance.
func NewRunner(cfg config.Config, backend Backend) *Runner {
	return &Runner{
		cfg:     cfg,
		backend: backend,
		nowFn:   time.Now,
	}
}

// Run executes the rollup process across [watermark, cutoff) split into 30-day slices.
func (r *Runner) Run(ctx context.Context, force bool) error {
	if !r.cfg.RollupEnabled {
		log.Println("rollup: disabled by configuration (ROLLUP_ENABLED=false)")
		return nil
	}

	now := r.nowFn()
	_, offsetSec := now.In(r.cfg.Location).Zone()
	if offsetSec%3600 != 0 {
		log.Printf("warning: APP_TIMEZONE %s has non-integer hour offset (%d sec); disabling rollup to prevent misalignment",
			r.cfg.Timezone, offsetSec)
		return nil
	}

	watermark, lastRunAt, minRawTs, err := r.backend.GetRollupState(ctx)
	if err != nil {
		return fmt.Errorf("failed to get rollup state: %w", err)
	}

	if !force && lastRunAt > 0 {
		elapsed := now.Unix() - lastRunAt
		minIntervalSec := int64(r.cfg.RollupMinIntervalHours * 3600)
		if elapsed < minIntervalSec {
			log.Printf("rollup: skipped (last run was %d sec ago, min interval is %d sec)", elapsed, minIntervalSec)
			return nil
		}
	}

	keepSec := int64(r.cfg.RollupKeepDays) * 86400
	cutoff := ((now.Unix() - keepSec) / 3600) * 3600

	from := watermark
	if from <= 0 {
		from = minRawTs
	}
	from = (from / 3600) * 3600

	if minRawTs <= 0 || from >= cutoff {
		log.Printf("rollup: nothing to aggregate (from=%d, cutoff=%d, minRawTs=%d)", from, cutoff, minRawTs)
		return nil
	}

	chunkSize := int64(30 * 86400) // 30 days per chunk
	curr := from

	for curr < cutoff {
		select {
		case <-ctx.Done():
			log.Printf("rollup: interrupted by context cancellation at %d", curr)
			return ctx.Err()
		default:
		}

		next := curr + chunkSize
		if next > cutoff {
			next = cutoff
		}

		log.Printf("rollup: aggregating window [%d, %d) (dry_run=%v)...", curr, next, r.cfg.RollupDryRun)
		if err := r.backend.RollupWindow(ctx, curr, next, r.cfg.RollupDryRun); err != nil {
			return fmt.Errorf("rollup chunk [%d, %d) failed: %w", curr, next, err)
		}

		curr = next
	}

	if !r.cfg.RollupDryRun {
		if err := r.backend.SetRollupLastRunAt(ctx, now.Unix()); err != nil {
			log.Printf("warning: failed to update rollup_last_run_at: %v", err)
		}
	}

	log.Printf("rollup: completed successfully up to cutoff %d (dry_run=%v)", cutoff, r.cfg.RollupDryRun)
	return nil
}
