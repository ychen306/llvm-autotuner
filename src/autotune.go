package main

import (
	"C"
	"bufio"
	"bytes"
	"errors"
	"flag"
	"fmt"
	"io/ioutil"
	"log"
	"math"
	"math/rand"
	"net"
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
	"unsafe"
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
	MAX_ITR_WITHOUT_CHANGE = 100 // quit after this many iterations without improvement
	SA_MAXOPTS             = 120
	T_MIN                  = 0.1
	T_MAX                  = 1
	INTERVAL               = 100 // decrease temperature at this interval
	ALPHA                  = 0.9
	REPLACE_RATE           = 0.2

	COMPILER_TIMEOUT = 30 * time.Second
)

var (
	numOpts int

	// command line arguments
	numWorkers  int
	opts        []string
	makefile    string
	exeVar      string
	objVar      string
	bcFile      string
	runRule     string
	verifyRule  string
	workerFile  string
	weightFile  string
	usingServer bool

	logfile *os.File
	errfile *os.File

	replayWorkers []string
	replayWeights []float64
)
var space *regexp.Regexp = regexp.MustCompile(`\s+`)
var spaceBegin *regexp.Regexp = regexp.MustCompile(`^\s+`)
var spaceEnd *regexp.Regexp = regexp.MustCompile(`\s+$`)
var newline *regexp.Regexp = regexp.MustCompile(`\n|\r\n|\r`)

func check(err error) {
	if err != nil {
		log.Fatal(err)
	}
}

func init() {
	flag.StringVar(&makefile, "makefile", "Makefile", "makefile used to build the executable")
	flag.StringVar(&exeVar, "exe-var", "EXE", "VARIABLE name used in makefile to hold the executable being built")
	flag.StringVar(&objVar, "obj-var", "OBJ", "VARIABLE name used in makefile to hold the bitcode file you wish to tune")
	flag.StringVar(&runRule, "run-rule", "run", "RULE used to run the executable in makefile")
	flag.StringVar(&verifyRule, "verify-rule", "verify", "RULE used to verify execution of the executable in makefile")
	flag.IntVar(&numWorkers, "w", (runtime.NumCPU()+1)/2, "number of workers")
	flag.BoolVar(&usingServer, "server", false, "use replay-server to speedup search")
	flag.StringVar(&workerFile, "worker-data", "worker-data.txt", "file listing path to unix sockets")
	flag.StringVar(&weightFile, "worker-weight", "worker-weight.txt", "file listing weights of which replay worker")

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
	check(err)

	errfile, err = os.Create(bcFile + ".tuning-err")
	check(err)

	replayWorkers, err = parseWorkers()
	check(err)

	replayWeights, err = parseWeights()
	check(err)

	numWorkers = 1
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
		log.Fatal(err)
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
	config.inlineThresh = 225
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
		if ii == numOpts {
			args[i+2] = "-verify"
		} else {
			args[i+2] = "-" + opts[ii]
		}
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
// the caller is responsible for deleting `obj` if `err` is not nil
func compile(config OptConfig) (obj TempFile, err error) {
	obj = getTempFile()

	optbc := getTempFile()
	defer optbc.delete()

	relocModel := "default"
	if usingServer {
		relocModel = "pic"
	}

	optArgs := append(config.asArgs(), bcFile, "-o", string(optbc))
	_, err = runCommand(exec.Command("opt", optArgs...), COMPILER_TIMEOUT)
	if err != nil {
		return
	}
	_, err = runCommand(exec.Command("llc",
		"-filetype=obj",
		"-relocation-model="+relocModel,
		string(optbc),
		"-o", string(obj)), COMPILER_TIMEOUT)
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
	obj, err := compile(config)
	if err != nil {
		err = &TuningError{config, OptError, err.Error()}
		return
	}
	defer obj.delete()

	if usingServer {
		// build shared library and run replay-workers
		lib := getTempFile()
		defer lib.delete()
		_, err = runCommand(exec.Command("make",
			"-f"+makefile,
			objVar+"="+string(obj),
			"LIB="+string(lib),
			string(lib)), -1)
		if err != nil {
			return
		}
		elapsed, err = runAllInvos(replayWorkers, string(lib))
		return
	}

	exe := getTempFile()
	defer exe.delete()

	// link `obj` to make `exe`
	_, err = runCommand(
		exec.Command("make",
			"-f"+makefile,
			exeVar+"="+string(exe),
			objVar+"="+string(obj),
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
	repProb := math.Max(t*REPLACE_RATE, 0.05)
	for i := range next.passes {
		if rand.Float64() > repProb {
			next.passes[i] = int(rand.Int31n(int32(numOpts) + 1))
		}
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
					itrWithoutChange = 0

					// keep track of the best result
					if cost < bestCost {
						var err error = nil

						// use this as a "checkpoint" and actually run the
						// best config found so far becase server
						// can't detect codegen error
						if usingServer {
							usingServer = false
							_, err = run(config, -1)
							usingServer = true
						}

						// roll back in case of codegen error
						if err != nil {
							cost = bestCost
							config = bestConfig
						} else {
							bestCost = cost
							bestConfig = config
						}
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
	if newCost == maxElapsed {
		return 0
	}
	if newCost < oldCost {
		ap = 1
	} else {
		// 1 unit = 0.5%
		//e := int(math.Log10(float64(oldCost)) - 1)
		//normalizedOld := float64(oldCost) / math.Pow10(e)
		//normalizedNew := float64(newCost) / math.Pow10(e)
		//diff := float64(normalizedOld - normalizedNew)
		diff := -1.
		ap = math.Exp(diff / t)
	}
	return
}

func loadopts() {
	// load options for `Config`
	optsFileContent, err := ioutil.ReadFile(OPTS_FILENAME)
	if err != nil {
		log.Fatal(err)
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
	var bestTime, o3Time time.Duration

	if !usingServer {
		bestConfigs := make(ConfigPool, rep)
		o3Configs := make(ConfigPool, rep)
		for i := 0; i < rep; i++ {
			bestConfigs[i] = best
			o3Configs[i] = o3
		}

		// get the average time
		bestTime = avg(bestConfigs.runAll(-1))
		o3Time = avg(o3Configs.runAll(-1))
	} else {
		o3Time, _ = run(o3, -1)
		bestTime, _ = run(best, -1)
	}

	fmt.Fprintln(logfile, "best time:", bestTime)
	fmt.Fprintln(logfile, "O3 time:", o3Time)
}

func parseWeights() (weights []float64, err error) {
	f, err := os.Open(weightFile)
	if err != nil {
		return
	}

	scanner := bufio.NewScanner(f)
	weights = make([]float64, 0, 4)
	for scanner.Scan() {
		var w float64
		w, err = strconv.ParseFloat(scanner.Text(), 64)
		if err != nil {
			err = errors.New("error parsing worker weight file")
			return
		}
		weights = append(weights, w)
	}
	return
}

func parseWorkers() (workers []string, err error) {
	f, err := os.Open(workerFile)
	if err != nil {
		log.Fatal(err)
	}
	scanner := bufio.NewScanner(f)
	workers = make([]string, 0, 4)
	for scanner.Scan() {
		workers = append(workers, scanner.Text())
	}
	err = scanner.Err()
	return
}

type response struct {
	success int32
	elapsed float64
	msg     [100 + 100]byte
}

// ask worker listening on `sockpath` to run function implemented in `libpath`
func runInvo(sockpath, libpath string) (elapsed time.Duration, err error) {
	// run the invocation `n` times
	n := 6
	stat := make([]int, n)

	var conn net.Conn
	for i := 0; i < n; i++ {
		conn, err = net.Dial("unix", sockpath)
		if err != nil {
			return
		}

		// send request
		fmt.Fprintf(conn, libpath)

		var resp response
		buf := make([]byte, unsafe.Sizeof(resp))
		addr := (*[unsafe.Sizeof(resp)]byte)(unsafe.Pointer(&resp))

		// get response
		_, err = bufio.NewReader(conn).Read(buf)
		if err != nil {
			return
		}

		copy((*addr)[:], buf)
		if resp.success == 1 {
			stat[i] = int(resp.elapsed)
		} else {
			msg := C.GoString((*C.char)(unsafe.Pointer(&resp.msg)))
			fmt.Fprintln(logfile, "shit:", msg)
			err = errors.New(msg)
			return
		}
	}
	sort.Ints(stat)
	sum := 0
	for _, e := range stat[1:n] {
		sum += e
	}
	elapsed = time.Duration(sum / (n - 2))

	return
}

func runAllInvos(replayWorkers []string, libpath string) (elapsed time.Duration, err error) {
	elapsed = 0
	for i, worker := range replayWorkers {
		var e time.Duration
		e, err = runInvo(worker, libpath)
		w := replayWeights[i]
		if err != nil {
			return
		}
		elapsed += time.Duration(float64(e) * w)
	}
	return
}

// kill the server
func killWorker(sockpath string) (err error) {
	conn, err := net.Dial("unix", sockpath)
	if err == nil {
		fmt.Fprintf(conn, "\x00")
	}
	return
}

func main() {
	defer logfile.Close()
	defer errfile.Close()
	defer func() {
		for _, w := range replayWorkers {
			err := killWorker(w)
			if err != nil {
				log.Fatal(err)
			}
		}
	}()

	best := run_sa()

	resultF, err := os.Create(bcFile + ".passes")
	if err != nil {
		log.Fatal(err)
	}
	fmt.Fprintln(resultF, best)
	resultF.Close()

	fmt.Fprintf(logfile, "\nbest:\n\t%v\n", best)
	logSpeedup(best)
}
