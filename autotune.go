package main

import (
	"bytes"
	"errors"
	"flag"
	"fmt"
	"io/ioutil"
	"math"
	"math/rand"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"time"
	"unicode"
)

const (
	OptError = "opt crash"

	CodegenError = "llc crash"

	IncorrectCode = "opt/llc generated incorrect code"

	BuildError = "build error"

	maxElapsed time.Duration = time.Duration(math.MaxInt64)
)

type TuningError struct {
	config OptConfig
	reason string
	detail string
}

func (err *TuningError) Error() string {
	return fmt.Sprintf("file: %s\nconfig: %v\nwhat: %s\ndetail:\n%s\n================================\n",
		bcFile, err.config, err.reason, err.detail)
}

const (
	OPTS_FILENAME = "opts.txt"

	// parameters for simulated annealing
	MAX_ITR_WITHOUT_CHANGE = 500 // quit after this many iterations without improvement
	SA_MAXOPTS             = 100
	T_MIN                  = 0.1
	T_MAX                  = 1.5
	INTERVAL               = 500 // decrease temperature at this interval
	ALPHA                  = 0.85
	REPLACE_RATE           = 0.5

	// parameters for genetic algorithm
	GA_MAXOPTS      = 120
	POPULATION_SIZE = 100
	ELITISM         = 0.1 // top 10% of the population survives without change
	MAX_GENERATION  = 100

	OPT_TIMEOUT = 30 * time.Second
)

var (
	numOpts int

	// command line arguments
	numWorkers int
	opts       []string
	makefile   string
	exeVar     string
	bcVar      string
	bcFile     string
	runRule    string
	verifyRule string

	logfile *os.File
	errfile *os.File
)
var space *regexp.Regexp = regexp.MustCompile(`\s+`)
var spaceBegin *regexp.Regexp = regexp.MustCompile(`^\s+`)
var spaceEnd *regexp.Regexp = regexp.MustCompile(`\s+$`)
var newline *regexp.Regexp = regexp.MustCompile(`\n|\r\n|\r`)

func init() {
	flag.StringVar(&makefile, "makefile", "Makefile", "makefile used to build the executable")
	flag.StringVar(&exeVar, "exe-var", "EXE", "VARIABLE name used in makefile to hold the executable being built")
	flag.StringVar(&bcVar, "bc-var", "BC", "VARIABLE name used in makefile to hold the bitcode file you wish to tune")
	flag.StringVar(&runRule, "run-rule", "run", "RULE used to run the executable in makefile")
	flag.StringVar(&verifyRule, "verify-rule", "verify", "RULE used to verify execution of the executable in makefile")
	//flag.StringVar(&strategy, "strat", "ga", "strategy to use")
	flag.IntVar(&numWorkers, "w", (runtime.NumCPU()+1)/2, "number of workers")

	flag.Parse()
	posArgs := flag.Args()

	if len(posArgs) != 1 {
		fmt.Fprintln(os.Stderr, "Usage: autotune [options] <bitcode file>")
		os.Exit(1)
	}
	bcFile = posArgs[0]
	if _, err := os.Stat(bcFile); err != nil {
		fmt.Fprintf(os.Stderr, "Bitcode file %s doesn't exist\n", bcFile)
		os.Exit(1)
	}

	rand.Seed(time.Now().UTC().UnixNano())

	loadopts()

	var err error
	logfile, err = os.Create(bcFile + ".tuning-log")
	if err != nil {
		panic(err)
	}

	errfile, err = os.Create(bcFile + ".tuning-err")
	if err != nil {
		panic(err)
	}
}

// perform a single point crossover
func (self Config) combineWith(other Config) Config {
	coPoint := rand.Int31n(int32(len(self.passes)))
	first := make([]int, coPoint, len(self.passes))
	copy(first, self.passes[:coPoint])
	second := other.passes[coPoint:]
	combined := Config{}
	combined.passes = append(first, second...)
	combined.inlineThresh = self.inlineThresh
	return combined
}

type OptConfig interface {
	asArgs() []string
}

// indices of configs
type Config struct {
	passes       []int
	inlineThresh int
}

type TempFile string

func getTempFile() (tf TempFile) {
	tempdir, err := ioutil.TempDir("/tmp", "autotune")
	if err != nil {
		panic(err)
	}
	tf = TempFile(tempdir + "/file")
	return
}

// rm a `TempFile` and it's parent directory from the system
func (tf TempFile) delete() error {
	return os.RemoveAll(filepath.Dir(string(tf)))
}

func randomConfig(max int) Config {
	config := Config{}
	config.inlineThresh = 325
	config.passes = make([]int, max)
	for i := 0; i < max; i++ {
		config.passes[i] = int(rand.Int31n(int32(numOpts)))
	}
	return config
}

func (config Config) String() (s string) {
	return strings.Join(config.asArgs(), " ")
}

// convert a configuration to arguments to feed `opt`
func (config Config) asArgs() (args []string) {
	args = make([]string, len(config.passes)+2)
	args[0] = "-mem2reg"
	args[1] = "-inline-threshold=" + strconv.Itoa(config.inlineThresh)

	for i, ii := range config.passes {
		args[i+2] = "-" + opts[ii]
	}
	return
}

// run a command, redirecting stdout and stderr to internal buffers
// in case of error, replace the content of err with stderr
func runCommand(cmd *exec.Cmd, timeout time.Duration) (out []byte, err error) {
	outBuf := bytes.Buffer{}
	cmd.Stderr = &outBuf
	cmd.Stdout = &outBuf

	timedout := false

	if timeout < 0 {
		err = cmd.Run()
	} else {
		done := make(chan error)

		go func() { done <- cmd.Run() }()

		// exit within time limit
		select {
		case err = <-done:
		case <-time.After(timeout):
			if cmd.Process != nil {
				cmd.Process.Kill()
			}
			err = errors.New("timeout")
			timedout = true
		}
	}

	out = outBuf.Bytes()

	if !timedout && err != nil {
		err = errors.New(string(out))
	}

	return
}

// apply `config` to `opt` and return path to the optimized bitcode file
//
// the caller is responsible for deleting `optbc` if `err` is not nil
func applyOpt(config OptConfig) (optbc TempFile, err error) {
	optbc = getTempFile()

	optArgs := append(config.asArgs(), bcFile, "-o", string(optbc))
	_, err = runCommand(exec.Command("opt", optArgs...), OPT_TIMEOUT)
	if err != nil {
		optbc.delete()
	}
	return
}

// lexer for a subset of shell command, assuming `s` is a VALID command
//
// sepcial characters: space, `"`, `'`, and `\` escaped character inside a string
func str2Command(s string) *exec.Cmd {
	args := make([]string, 0, 42)
	l := len(s)
	// flag indicating if lexing a token (i.e. "inside" a token)
	intok := false
	begin := 0
	for i := 0; i < l; i++ {
		c := s[i]
		if unicode.IsSpace(rune(c)) {
			if intok {
				// end of token
				intok = false
				args = append(args, s[begin:i])
			}
		} else if c == '"' || c == '\'' {
			// skip quote
			i++

			quote := c
			quoted := make([]uint8, 0, l)

			for ; s[i] != quote; i++ {
				// skip to escaped character
				if s[i] == '\\' {
					i++
				}
				quoted = append(quoted, s[i])
			}
			args = append(args, string(quoted))

			// skip quote
			i++
		} else if !intok {
			// begin of token
			begin = i
			intok = true
		}
	}

	return exec.Command(args[0], args[1:]...)
}

type O3 struct{}

func (_ O3) asArgs() []string { return []string{"-O3"} }

// compile and run a configuration, return how much it takes to run
func run(config OptConfig, timeout time.Duration) (elapsed time.Duration, err error) {
	optbc, err := applyOpt(config)
	if err != nil {
		err = &TuningError{config, OptError, err.Error()}
		return
	}
	defer optbc.delete()

	exe := getTempFile()
	defer exe.delete()

	// compile `optbc` to `exe`
	_, err = runCommand(
		exec.Command("make",
			"-f"+makefile,
			exeVar+"="+string(exe),
			bcVar+"="+string(optbc),
			string(exe)), -1)

	if err != nil {
		err = &TuningError{config, CodegenError, err.Error()}
		return
	}

	// tell the user to dump output to this file if it dosen't use stdout
	outF := getTempFile()
	defer outF.delete()

	// extract the command to run `exe` from makefile and
	// run the command ourselves so that time can be measured
	// more accurately
	runStr, err := runCommand(
		exec.Command("make",
			"-f"+makefile,
			"OUT="+string(outF),
			exeVar+"="+string(exe),
			runRule,
			"--just-print"), -1)
	if err != nil {
		err = &TuningError{config, BuildError, err.Error()}
		return
	}

	// actually run the command
	runCmd := str2Command(string(runStr))
	out, err := runCommand(runCmd, timeout)
	if err != nil {
		err = &TuningError{config, IncorrectCode, err.Error()}
		return
	}

	// time the command
	elapsed = runCmd.ProcessState.SystemTime() + runCmd.ProcessState.UserTime()

	// dump stdout to a tempfile so that the verification command can use it
	stdoutF := getTempFile()
	defer stdoutF.delete()
	ioutil.WriteFile(string(stdoutF), out, 0666)

	// verify
	_, err = runCommand(
		exec.Command("make",
			"-f"+makefile,
			"OUT="+string(outF),
			"STDOUT="+string(stdoutF),
			exeVar+"="+string(exe),
			verifyRule), -1)
	if err != nil {
		err = &TuningError{config, IncorrectCode, err.Error()}
	}

	return
}

func trimAndSplit(s string) []string {
	trimmed := spaceBegin.ReplaceAllString(spaceEnd.ReplaceAllString(s, ""), "")
	return space.Split(trimmed, -1)
}

// mutate a configuration slightly
func (config Config) randNext(t float64) Config {
	next := config
	next.passes = make([]int, len(config.passes))
	copy(next.passes, config.passes)
	mutateProb := math.Max(t*REPLACE_RATE, 0.05)
	for i := 0; i < len(config.passes); i++ {
		if mutateProb > rand.Float64() {
			next.passes[i] = int(rand.Int31n(int32(numOpts)))
		}
	}
	r := rand.Float32()
	if r > 0.5*2.0/3.0 {
		next.inlineThresh += 50
	} else if r > 0.5*1.0/3.0 && next.inlineThresh > 100 {
		next.inlineThresh -= 50
	}
	return next
}

// similar to run;
// sets elapsed to max float and logs the error in case of error
func checkRun(config OptConfig, timeout time.Duration) (elapsed time.Duration) {
	elapsed, err := run(config, timeout)
	if err != nil {
		elapsed = maxElapsed
		fmt.Fprintln(errfile, err)
	}
	return
}

// simmulated annealing
func run_sa() Config {
	numWorkers = 1
	// result of speculatively executing `itr`
	type Result struct {
		config   Config
		accepted bool
		itr      int
		cost     time.Duration
	}

	config := randomConfig(SA_MAXOPTS)

	results := make(chan Result)
	var t float64
	cost := checkRun(config, -1)
	bestConfig := config
	bestCost := cost
	var itr int

	// mutate `config` and send the result to `results`
	mutateAndRun := func(itr int) {
		mutated := config.randNext(t)
		newCost := checkRun(mutated, bestCost*4)
		ap := getAcceptanceProb(cost, newCost, t)
		results <- Result{
			config:   mutated,
			cost:     newCost,
			accepted: ap > rand.Float64(),
			itr:      itr}
	}

	// mapping iteration -> result
	sortedResults := make([]Result, numWorkers)
	itrWithoutChange := 0

	for t = T_MAX; t > T_MIN; t *= ALPHA {
		itr = 0
		for itr < INTERVAL {
			for i := 0; i < numWorkers; i++ {
				go mutateAndRun(i)
			}
			// wait for all workers to finish and find the first accepted result
			for i := 0; i < numWorkers; i++ {
				r := <-results
				sortedResults[r.itr] = r
			}

			var result Result
			for i := 0; i < numWorkers; i++ {
				result = sortedResults[i]
				itrWithoutChange += 1
				if result.accepted {
					config = result.config
					cost = result.cost

					// keep track of the best result
					if cost < bestCost {
						itrWithoutChange = 0
						bestCost = cost
						bestConfig = config
					}

					break
				}
			}

			if itrWithoutChange >= MAX_ITR_WITHOUT_CHANGE {
				return bestConfig
			}

			itr += (result.itr + 1)

			fmt.Fprintf(logfile, "%d: %v (best = %v)\n", itr, cost, bestCost)
		}
	}
	return bestConfig
}

func getAcceptanceProb(oldCost, newCost time.Duration, t float64) (ap float64) {
	if newCost < oldCost {
		ap = 1
	} else {
		// 1 unit = 0.5%
		diff := float64(oldCost-newCost) / float64(oldCost) * 200
		ap = math.Exp(diff / t)
	}
	return
}

func loadopts() {
	// load options for `Config`
	optsFileContent, err := ioutil.ReadFile(OPTS_FILENAME)
	if err != nil {
		panic(err)
	}
	opts = trimAndSplit(string(optsFileContent))
	numOpts = len(opts)
}

// a thread pool that can only run configs...
type ConfigPool []OptConfig

type Result struct {
	config  OptConfig
	elapsed time.Duration
}

// run all jobs; running `numWorkers` jobs in parallel
func (p *ConfigPool) runAll(timeout time.Duration) (results []Result) {
	resChan := make(chan Result)
	results = make([]Result, 0, len(*p))

	// wait for a spot to open up to run more jobs
	recv := func() {
		res := <-resChan
		results = append(results, res)
	}

	// start a job and forget about it
	numRunning := 0
	dispatch := func(config OptConfig) {
		go func() {
			elapsed := checkRun(config, timeout)
			resChan <- Result{config, elapsed}
		}()
	}

	// start all the jobs as soon as possible
	for _, config := range *p {
		if numRunning == numWorkers {
			recv()
			numRunning -= 1
		}
		dispatch(config)
		numRunning += 1
	}

	// wait for all jobs to finish
	for len(results) < len(*p) {
		recv()
	}

	return
}

type byTime []Result

func (rs byTime) Len() int           { return len(rs) }
func (rs byTime) Swap(i, j int)      { rs[i], rs[j] = rs[j], rs[i] }
func (rs byTime) Less(i, j int) bool { return rs[i].elapsed < rs[j].elapsed }

// given a slice of Result sorted by time,
// randomly pick a config, whose probability of being
// picked is inversely proportional to the time it
// takes to run
func pickOne(results []Result) (config Config) {
	probs := make([]float32, len(results))
	scores := make([]float32, len(results))
	var totalScore float32 = 0.0
	for i, res := range results {
		score := 1 / (float32(res.elapsed) / 1e6)
		totalScore += score
		scores[i] = score
	}
	for i, score := range scores {
		probs[i] = score / totalScore
	}

	r := rand.Float32()

	var floor float32 = 0.0
	i := 0
	for r >= floor {
		floor += probs[i]
		i++
	}

	return results[i-1].config.(Config)
}

// TODO
// consider expire some entries

// cache to check if the config has been tested yet
type ConfigCache map[string]bool

func (cache ConfigCache) add(results []Result) {
	for _, result := range results {
		cache[strings.Join(result.config.asArgs(), "")] = true
	}
}

func (cache ConfigCache) find(config Config) bool {
	key := strings.Join(config.asArgs(), "")
	return cache[key]
}

// genetic algorithm
func run_ga() (best Config) {
	// our initial population
	configs := make(ConfigPool, POPULATION_SIZE)
	for i := range configs {
		configs[i] = randomConfig(GA_MAXOPTS)
	}
	cache := make(ConfigCache)

	var bestTime time.Duration = -1

	for i := 0; i < MAX_GENERATION; i++ {
		// timeout with the 4 times that of the best time
		var timeout time.Duration
		if bestTime != maxElapsed {
			timeout = bestTime * 4
		} else {
			// previous generation all timeout'd
			timeout = -1
		}
		results := configs.runAll(timeout)
		sort.Sort(byTime(results))
		cache.add(results)

		bestTime = results[0].elapsed
		best = results[0].config.(Config)
		fmt.Fprintln(logfile, bestTime)

		// next generation
		next := make(ConfigPool, POPULATION_SIZE)

		// the elites survive without change
		eliteIdx := int(float32(len(configs)) * ELITISM)
		for i := 0; i < eliteIdx; i++ {
			next[i] = results[i].config
		}

		// replace the rest of the population by repeatedly
		// randomly selecting and combining two configs from the pool,
		// whose possibility of being selected is proportional to its fitness
		// the combined configs has a probability to "mutate"
		for i := eliteIdx; i < len(next); i++ {
			cfgA := pickOne(results)
			cfgB := pickOne(results)
			newCfg := cfgA.combineWith(cfgB).randNext(0.2)
			// if the config has already been tried, replace it by a mutated version
			for cache.find(newCfg) {
				newCfg = newCfg.randNext(0.2)
			}
			next[i] = newCfg
		}

		configs = next
	}
	return
}

func avg(results []Result) time.Duration {
	// discard the best and worst result
	sort.Sort(byTime(results))
	results = results[1:len(results)]

	var total time.Duration = 0
	for _, r := range results {
		total += r.elapsed
	}
	return total / time.Duration(len(results))
}

// log the speedup relative to O3
func logSpeedup(best Config) {
	rep := 10
	o3 := O3{}
	bestConfigs := make(ConfigPool, rep)
	o3Configs := make(ConfigPool, rep)
	for i := 0; i < rep; i++ {
		bestConfigs[i] = best
		o3Configs[i] = o3
	}

	// get the average time
	bestTime := avg(bestConfigs.runAll(-1))
	o3Time := avg(o3Configs.runAll(-1))

	fmt.Fprintln(logfile, "best time:", bestTime)
	fmt.Fprintln(logfile, "O3 time:", o3Time)
}

func main() {
	defer logfile.Close()
	defer errfile.Close()

	best := run_sa()

	resultF, err := os.Create(bcFile + ".passes")
	if err != nil {
		panic(err)
	}
	fmt.Fprintln(resultF, best)
	resultF.Close()

	fmt.Fprintf(logfile, "\nbest:\n\t%v\n", best)
	logSpeedup(best)
}
