/**
 * Main function. Generates ec and tsv files.
 */
#include <iostream>           // warning and message i/o
#include <string>
#include <vector>
#include <time.h>             // provides total time elapsed
#include <getopt.h>           // getopt_long to parse commandline args
#include <set>

#include "structs.hpp"
#include "file_io.hpp"
#include "util.hpp"
#include "kallisto_util.hpp"
#include "TCC_Matrix.hpp"

using namespace std;

/**
 * Parse commandline arguments. Test-open relevant files and abort if a file
 * fails to open. Allocate relevant data structures and fill with information
 * from GTFs and SAMs. Write to output files, deallocate data structures, and 
 * return.
 */
int main(int argc, char **argv) {
    string usage = "Usage:\n  " + string(argv[0]) + " [options]* -g <GTF> ";
    usage += "-S <SAM> [-o <output>]\n\n";
    usage += "  <GTF>               Comma-separated list of GTF gene ";
    usage +=                       "annotation files\n";
    usage += "  <SAM>               Comma-separated list of ";
    usage +=                       "sam files containing aligned ";
    usage +=                       "single-end reads\n";
    usage += "  <output>            Name of output file (defaults to ";
    usage +=                       "out.ec, out.tsv)\n\n";
    usage += "Options:\n";
    usage += "  --unmatched-output <SAM>   Output unmatched reads to ";
    usage +=                              "file <SAM>. Default ";
    usage +=                              "setting ignores these reads.\n";
    usage += "  -q                         Suppresses some warnings and ";
    usage +=                              "status updates\n";
    usage += "  -transcriptome <fa>        Change TCC numbering so it ";
    usage +=                              "those that would be generated by ";
    usage +=                              "kallisto using transcriptome(s) ";
    usage +=                              "<fa>. Takes a comma-separated ";
    usage +=                              "of file names\n";
    usage += "  -ec <ec>                   Output TCCs in the same order as in";
    usage +=                              " output file ec.\n";

    /* Parse commandline arguments */
    vector<string> gtf_files; // annotated sequence files
    vector<string> sam_files; // files with information about reads
    vector<string> transcriptome_files; // optional--transcriptomes input into
                                        // kallisto, so that this program will
                                        // modify its own output so that TCC
                                        // numbering matches
    string kallisto_ec; // a kallisto EC file, for use if user wants TCC indexes
                        // to match those of this kallisto output
    
    string out_name = "out", unmatched_out = ""; // default outfile names
    int err, verbose = 1, unmatched = 0; // various variables for use later
    time_t start = time(0); // to calculate total runtime

    // for use with getopt_long
    struct option long_opts[] = {
        {"quiet", no_argument, no_argument, 'q'},
        {"unmatched-output", required_argument, 0, 'u'},
        {"gtf", required_argument, 0, 'g'},
        {"sam", required_argument, 0, 's'},
        {"output", required_argument, 0, 'o'},
        {"transcriptome", required_argument, 0, 't'},
        {"kallisto-ec", required_argument, 0, 'e'}
    };

    // use getopt_long to parse arguments
    int opt_index = 0;
    while(1) {
        int c = getopt_long(argc, argv, "qu:g:S:o:t:e:", long_opts, &opt_index);
        if (c == -1) { break; }
        switch (c) {
            case 'q':   verbose = 0;
                        break;
            case 'u':   unmatched_out = optarg;
                        break;
            case 'g':   gtf_files = parse_csv(optarg);
                        break;
            case 'S':   sam_files = parse_csv(optarg);
                        break;
            case 'o':   out_name = optarg;
                        break;
            case 't':   transcriptome_files = parse_csv(optarg);
                        break;
            case 'e':   kallisto_ec = optarg;
                        break;
        }
    }

    // must have gtf and sam files! if not, program prints usage and exits
    if (gtf_files.size() == 0 || sam_files.size() == 0) {
        cerr << usage << flush;
        return 1;
    }


    /* Check that all files are valid by trying to open them */
    cout << "Checking that all files are valid ";
    cout << "and clearing output files... " << flush;
    for (uint i = 0; i < gtf_files.size(); ++i) {
        if (!test_open(gtf_files[i])) {
            cerr << endl << "  ERROR: Failed to open " << gtf_files[i] << endl;
            return 1;
        }
    }
    for (uint i = 0; i < sam_files.size(); ++i) {
        if (!test_open(sam_files[i])) {
            cerr << endl <<  "  ERROR: failed to open " << sam_files[i] << endl;
            return 1;
        }
    }
    if (unmatched_out.size() != 0 && !test_open(unmatched_out, 1)) {
        cerr << endl << "  ERROR: failed to open " << unmatched_out << endl;
        return 1;
    }
    if (transcriptome_files.size() != 0) {
        for (uint i = 0; i < transcriptome_files.size(); ++i) {
            if (!test_open(transcriptome_files[i])) {
                cerr << endl << "  ERROR: failed to open ";
                cerr << transcriptome_files[i] << endl;
                return 1;
            }
        }
    }
    if (!test_open(out_name + ".ec", 1)) {
        cerr << endl << "  ERROR: failed to open " << out_name << ".ec" << endl;
        return 1;
    }
    if (!test_open(out_name + ".tsv", 1)) {
        cerr << endl << "  ERROR: failed to open " << out_name << ".tsv" << endl;
        return 1;
    }
    cout << "success!" << endl;


    /* Start reading files and filling in TCC matrix */
        
    // Initialize relevant data structures.
    
    // Each vector<Exon*> holds pointers to every exon in one chromosome or
    // scaffold with no duplicates. So, exons holds all exons for an organism,
    // sorted by chromosome/scaffold.
    vector<vector<Exon>*> *exons = new vector<vector<Exon>*>;
    
    // An object in which the data of the TCC matrix resides.
    TCC_Matrix *matrix = new TCC_Matrix(sam_files.size());

    // read GTFs and fill in vector<vector<Exon*>*> completely
    cout << "Reading GTFs... " << endl;
    err = readGTFs(gtf_files, transcriptome_files, *exons, verbose);
    if (err == 1) {
        // No need to print an error message--readGTFs does it for us.
        return 1;
    }
    cout << "done" <<  endl;

    // Write a header for unmatched_out if necessary. we do so here since
    // we need it to preface any data.
    // TODO: put all this code in its own function! also, you're currently
    // printing mutliple @HD lines--there can only be one...
    if (unmatched_out.size() != 0) {
        cout << "Writing header for unmatched output file... " << flush;
        ofstream outfile(unmatched_out);
        if (!outfile.is_open()) {
            cerr << "ERROR: Failed to open " << unmatched_out << endl;
            return 1;
        }
        for (uint i = 0; i < sam_files.size(); ++i) {
            ifstream infile(sam_files[i]);
            if (!infile.is_open()) {
                cerr << "ERROR: Failed to open " << sam_files[i] << endl;
                return 1;
            }
            string inp;
            while (getline(infile, inp)) {
                if (inp.size() < 2) {
                    continue;
                }
                if (inp.substr(0, 3).compare("@HD") == 0) {
                    outfile << inp << endl;
                }
                else if (inp.substr(0, 3).compare("@SQ") == 0) {
                    outfile << inp << endl;
                }
                else {
                    break;
                }
            }
            infile.close();
        }
        outfile << "@PG\tID:[unknown]\tPN:[unknown]\tVN:1.0\tCL:\"";
        for (int i = 0; i < argc - 1; ++i) {
            outfile << argv[i] << " ";
        }
        outfile << argv[argc - 1] << "\"\n";
        outfile.close();
        cout << "done" << endl;
    }
    // Read SAM files and fill in TCC matrix appropriately.
    cout << "Reading SAM... " << endl;
    for (uint i = 0; i < sam_files.size(); ++i) {
        uint64_t temp = readSAM(sam_files[i], i, *exons, *matrix,
                                     unmatched_out, verbose);
        if (err == -1) {
            cerr << "ERROR: Failed to open " << sam_files[i] << endl;
            return 1;
        }
        unmatched += temp;
    }
    cout << "done" << endl;

        
    /* Write to output files */
    cout << "Writing to file... " << flush;
    if (kallisto_ec.size() == 0) {
        err = matrix->write_to_file(out_name);
    }
    else {
        vector<string> *kallisto_order = new vector<string>;
        set<string> *kallisto_ecs = new set<string>;
        err = get_kallisto_ec_order(kallisto_ec, *kallisto_order,
                                    *kallisto_ecs);
        // Only continue if previous function was successful.
        if (err != 1) {
            err = matrix->write_to_file_in_order(out_name, *kallisto_order,
                                                 *kallisto_ecs);;
        }
        delete kallisto_order;
        delete kallisto_ecs;
    }
    if (err == 1) {
        cerr << "ERROR: Failed to open outfile(s) " << out_name << ".ec or ";
        cerr << out_name << ".tsv" << endl;
    }
    cout << "done" << endl;



    /* Clean-up */
    for (uint i = 0; i < exons->size(); ++i) {
        delete (*exons)[i];
    }
    delete exons;
    delete matrix;


    /* Print total runtime of program and number of unmatched reads */
    cout << "Time: ";
    time_t diff = time(0) - start;
    string s = to_string((int)(diff / 3600));
    if (s.size() == 1) {
        cout << "0";
    }
    cout << s << ":";
    s = to_string((int)((diff % 3600) / 60));
    if (s.size() == 1) {
        cout << "0";
    }
    cout << s << ":";
    s = to_string((int)((diff % 3600) % 60));
    if (s.size() == 1) {
        cout << "0";
    }
    cout << s << '\t';

    cout << "Unmatched reads: " << unmatched << endl;

    return 0;
}
