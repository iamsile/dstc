#include <belief-tracker.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <set>
#include <chrono>
#include <boost/filesystem.hpp>

typedef std::chrono::high_resolution_clock clock_type;

namespace info = belief_tracker::info::single_info;
typedef belief_tracker::joint::sum::Belief belief_type;
namespace this_reference = belief_tracker::this_reference::single;

/*
namespace info = belief_tracker::info::multi_info;
typedef belief_tracker::joint::sum::Belief belief_type;
namespace this_reference = belief_tracker::this_reference::weak_dontcare;
*/

int main(int argc, char * argv[]) {

  if(argc != 5 && argc != 6) {
    std::cerr << "Usage : " << argv[0] << " filename.flist ontology belief_thr verbose(0,1) <session-id>" << std::endl;
    return -1;
  }

  auto clock_begin = clock_type::now();

  std::string flist_filename = argv[1];
  std::string ontology_filename = argv[2];
  double belief_thr = atof(argv[3]);
  bool verbose = atoi(argv[4]);  

  bool process_a_single_dialog = false;
  std::string session_to_process;
  if(argc == 6) {
    process_a_single_dialog = true;
    session_to_process = std::string(argv[5]);
  }

  // We define the set of rules to be used for extracting the informations
  std::list<belief_tracker::info::single_info::rules::rule_type> rules {
    belief_tracker::info::single_info::rules::inform,
      belief_tracker::info::single_info::rules::explconf,
      belief_tracker::info::single_info::rules::implconf,
      belief_tracker::info::single_info::rules::negate,
      belief_tracker::info::single_info::rules::deny
      };



  // We parse the ontology
  belief_tracker::Ontology ontology = belief_tracker::parse_ontology_json_file(ontology_filename);
  // Initialize the static fields of the converter
  belief_tracker::Converter::init(ontology);

  // We parse the flist to access to the dialogs
  if(verbose) std::cout << "Parsing the flist file for getting the dialog and label files" << std::endl;
  auto dialog_label_fullpath = belief_tracker::parse_flist(flist_filename);
  if(verbose) std::cout << "I parsed " << dialog_label_fullpath.size() << " dialogs (and labels if present) " << std::endl;
  if(verbose) std::cout << "done " << std::endl;

  // Compute and set the dataset name
  boost::filesystem::path pathname(flist_filename);
  std::string dataset_name = pathname.filename().string();
  int lastindex = dataset_name.find_last_of("."); 
  dataset_name = dataset_name.substr(0, lastindex); 

  // The stream for saving the tracker output
  std::ostringstream ostr;
  ostr.str("");
  ostr << dataset_name << "-output.json";
  std::ofstream outfile_tracker(ostr.str().c_str());

  // Let us begin the serial save
  belief_tracker::serial::begin_dump(outfile_tracker, dataset_name);
  std::cout << "The tracker output will be saved to " << ostr.str() << std::endl; 

  std::ofstream outfile_size("belief-size.data");
  std::cout << "The size of the belief will be dumped in belief-size.data" << std::endl;

  unsigned int dialog_index = 0;
  unsigned int number_of_dialogs  = dialog_label_fullpath.size();
  unsigned int nb_turns;

  // Iterate over the dialogs
  for(auto& dlfpi: dialog_label_fullpath) {

    std::string dialog_fullpath = dlfpi[0];
    //std::string label_fullpath = dlfpi[1];
    std::string session_id = dlfpi[2];
    std::string dialog_entry = dlfpi[3];

    if(process_a_single_dialog && session_to_process != session_id)
      continue;

    belief_tracker::Dialog dialog;

    std::list<belief_tracker::DialogTurn>::iterator dialog_turn_iter;
    std::list<belief_tracker::DialogAct> prev_mach_acts;

    dialog = belief_tracker::parse_dialog_json_file(dialog_fullpath);

    std::cout << '\r' << "Processing dialog " << (dialog_index +1)<< "/" << number_of_dialogs << std::flush;
    if(verbose) std::cout << session_id << std::endl;
    if(verbose)  std::cout << std::endl;


    ////////////////////////////////////
    // Belief initialization
    belief_type belief;
    belief.start();
    if(verbose) std::cout << "Initial belief : " << std::endl << belief << std::endl;
    
    std::map< std::string, double > requested_slots;
    std::map< std::string, double > methods;
    methods["byalternatives"] = 0.0;
    methods["byconstraints"] = 0.0;
    methods["byname"] = 0.0;
    methods["finished"] = 0.0;
    methods["none"] = 1.0;


    /////////////////////
    // Some initializations before looping over the turns
    nb_turns = dialog.turns.size();
    dialog_turn_iter = dialog.turns.begin();

    std::vector<belief_tracker::TrackerSessionTurn> tracker_session;

    // //////////////////////////////////////////////
    // // We now iterate over the turns of the dialog

    for(unsigned int turn_index = 0; turn_index < nb_turns ; ++turn_index, ++dialog_turn_iter) {
      if(verbose) {
    	std::cout << "****** Turn " << turn_index << " ***** " << dialog.session_id << std::endl;
    	std::cout << std::endl;
    	std::cout << "Dialog path : " << dialog_fullpath << std::endl;
    	std::cout << std::endl;
      }
      auto& slu_hyps = dialog_turn_iter->user_acts;
      auto& macts = dialog_turn_iter->machine_acts;

      if(verbose) std::cout << "Received machine act : " << belief_tracker::dialog_acts_to_str(macts) << std::endl;
      if(verbose) std::cout << "Received hypothesis : " << std::endl << belief_tracker::slu_hyps_to_str(slu_hyps) << std::endl;

      /////// Preprocessing of the machine utterance and SLU hypothesis
      // - Rewriting the repeat() act with the act of the previous turn
      // - Renormalizing the SLU (they do not always sum to 1)
      // - Solving the reference of "this" in the SLU
      belief_tracker::rewrite_repeat_machine_act(macts, prev_mach_acts);
      belief_tracker::renormalize_slu(slu_hyps);
      this_reference::rewrite_slu(slu_hyps, macts);

      // We can now proceed by extracting the informations from the SLU
      auto turn_info = info::extract_info(slu_hyps, macts, rules, verbose);

      // We now use the scored info to update the probability distribution
      // over the values of each slot
      // This produces the tracker output for this dialog
      // We make use of a lazy representation, i.e. we
      // just represent the slot/value pairs for which the belief is != 0
      belief = belief.update(turn_info, belief_tracker::info::transition_function);

      // And the belief is cleaned according to the provided threshold
      // for the DSTC3 with the SJTU SLU, the belief can become really large
      belief.clean(belief_thr);

      // Update the requested slots and methods
      belief_tracker::requested_slots::update(slu_hyps, macts, requested_slots);
      belief_tracker::methods::update(slu_hyps, macts, methods);

      // We can now compute some statistics given the labels
      auto best_goal = belief.extract_best_goal();

      /****** For debugging  ******/
      if(verbose) {
    	std::cout << "Machine act : " << belief_tracker::dialog_acts_to_str(macts) << std::endl;
    	std::cout << "SLU hyps : " << std::endl;
    	std::cout << belief_tracker::slu_hyps_to_str(slu_hyps) << std::endl;
    	std::cout << "Extracted Turn info : " << std::endl;
    	std::cout << turn_info << std::endl;
    	std::cout << "Belief :" << std::endl;
    	std::cout <<  belief << std::endl;
    	std::cout << "Best goal : " << best_goal.toStr() << std::endl;
    	std::cout << "Methods : " << belief_tracker::methods::toStr(methods) << std::endl;
    	std::cout << "Requested slots : " << belief_tracker::requested_slots::toStr(requested_slots) << std::endl;
      }
      /******* END DEBUG *****/
      if(verbose) std::cout << std::endl;
      
      outfile_size << belief.belief.size() << std::endl;

      belief_tracker::TrackerSessionTurn tracker_session_turn;
      belief.fill_tracker_session(tracker_session_turn);
      tracker_session_turn.requested_slots_scores = requested_slots;
      tracker_session_turn.method_labels_scores = methods;
      tracker_session.push_back(tracker_session_turn);
      
      // Copy the previous machine acts in order to replace 
      // a repeat if any
      prev_mach_acts = macts;

    } // End of looping over the turns

    // Dump the current session
    belief_tracker::serial::dump_session(outfile_tracker, dialog.session_id, tracker_session, dialog_index == (number_of_dialogs - 1));
    
    if(process_a_single_dialog && session_to_process == dialog.session_id) {
      break;
    }
    
    ++dialog_index;
  }
  std::cout << std::endl;




  // Take the clock for computing the walltime
  auto clock_end = clock_type::now();
  double walltime = std::chrono::duration_cast<std::chrono::seconds>(clock_end - clock_begin).count();

  // close the file in which the size of the belief is saved
  outfile_size.close();

  // And finally we finish to fill the tracker output
  belief_tracker::serial::end_dump(outfile_tracker, walltime);
  outfile_tracker.close();

}



