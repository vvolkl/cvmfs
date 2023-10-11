package main

import (
  "bufio"
  "fmt"
  "io"
  "os"
  "log"
  "time"
  "flag"
  "strings"
  "os/exec"
  "syscall"
)

func ReadChanges(file *os.File) chan string {

    changes := make(chan string)

    file.Seek(0, os.SEEK_END)
    bf := bufio.NewReader(file)

    go func() {
        for {
            line, _, err := bf.ReadLine()
            if len(line) != 0 {
                changes <- string(line)
            } else if err == io.EOF {
                time.Sleep(1 * time.Second)
            }
        }
    }()
    return changes
}

func ProcessRequest(logfile_name string, file_name string, repository_name string, rotation int) {

    file, err := os.OpenFile(file_name, os.O_RDONLY, 0755)
    if err != nil {
        log.Fatalf("OpenFile: %s", err)
    }

    if rotation == 1 {
        // To make sure we haven't missed any image during rotation
        scanner := bufio.NewScanner(file)
        scanner.Split(bufio.ScanLines)
        for scanner.Scan() {
            ExecDucc(scanner.Text(), logfile_name, repository_name)
        }
        rotation = 0
    }

    changes := ReadChanges(file)

    for {

        msg := <-changes

        if msg == "xx|file rotation|xx" {
                file.Close()
                rotation = 1
                ProcessRequest(logfile_name, file_name, repository_name, rotation)
        }

        ExecDucc(msg, logfile_name, repository_name)
    }
}

func ExecDucc(msg string, logfile_name string, repository_name string) {

    msg_split := strings.Split(msg, "|")
    image := msg_split[len(msg_split)-1]
    image = strings.ReplaceAll(image, "https://", "")
    action := msg_split[len(msg_split)-2]

    if action == "push" {
        numberOfExecutions := 3
        for i := 0; i < numberOfExecutions; i++ {
                fmt.Printf("DUCC ingestion n.%d for %s started...\n", i+1, image)
                _, err := exec.Command("cvmfs_ducc", "convert-single-image", "-n", logfile_name, "-p", image, repository_name, "--skip-thin-image", "--skip-podman").Output()
                if err != nil {
                        log.Fatal(err)
                }
                fmt.Printf("[done ingestion n.%d]\n",i+1)
        }
    }
}

func main() {

    var rotation int

    logfile_name := flag.String("log_file", "ducc-conversion.log", "DUCC log file")
    file_name := flag.String("notifications_file", "notifications.txt", "Notification file")
    repository_name := flag.String("repository_name", "unpacked.infn.it", "Repository")
    flag.Parse()

    lname := *logfile_name
    fname := *file_name
    rname := *repository_name

// create the notifications file with the g+w permission
    originalUmask := syscall.Umask(0)
    syscall.Umask(0o002)
    file, err := os.OpenFile(fname, os.O_RDONLY|os.O_CREATE, 0775)
    if err != nil {
        log.Fatalf("OpenFile: %s", err)
    }
    file.Close()
    syscall.Umask(originalUmask)

    ProcessRequest(lname, fname, rname, rotation)
}
