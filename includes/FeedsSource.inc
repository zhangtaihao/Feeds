<?php
// $Id: FeedsSource.inc,v 1.26.2.1 2010/09/29 23:56:10 alexb Exp $

/**
 * @file
 * Definition of FeedsSourceInterface and FeedsSource class.
 */

/**
 * Declares an interface for a class that defines default values and form
 * descriptions for a FeedSource.
 */
interface FeedsSourceInterface {

  /**
   * Crutch: for ease of use, we implement FeedsSourceInterface for every
   * plugin, but then we need to have a handle which plugin actually implements
   * a source.
   *
   * @see FeedsPlugin class.
   *
   * @return
   *   TRUE if a plugin handles source specific configuration, FALSE otherwise.
   */
  public function hasSourceConfig();

  /**
   * Return an associative array of default values.
   */
  public function sourceDefaults();

  /**
   * Return a Form API form array that defines a form configuring values. Keys
   * correspond to the keys of the return value of sourceDefaults().
   */
  public function sourceForm($source_config);

  /**
   * Validate user entered values submitted by sourceForm().
   */
  public function sourceFormValidate(&$source_config);

  /**
   * A source is being deleted.
   */
  public function sourceSave(FeedsSource $source);

  /**
   * A source is being saved.
   */
  public function sourceDelete(FeedsSource $source);
}

/**
 * Status of an import or clearing operation on a source.
 */
class FeedsState {
  /**
   * Natural numbers denoting the total and the progress of that total made.
   */
  public $total;
  public $progress;

  /**
   * Used as a pointer to store where left off. Must be serializable.
   */
  public $pointer;

  /**
   * Natural numbers denoting more details about the progress being made.
   */
  public $created;
  public $updated;
  public $deleted;
  public $skipped;
  public $failed;

  /**
   * Constructor, initialize variables.
   */
  public function __construct() {
    $this->total =
    $this->progress = 1;
    $this->created =
    $this->updated =
    $this->deleted =
    $this->skipped =
    $this->failed = 0;
  }

  /**
   * Report normalized progress.
   *
   * @return
   *   A float between 0 and 1, 1 = FEEDS_BATCH_COMPLETE.
   */
  public function progress() {
    $progress = $this->progress / $this->total;
    if ($progress == FEEDS_BATCH_COMPLETE && $this->total != $this->progress) {
      return 0.999;
    }
    return $progress;
  }
}

/**
 * This class encapsulates a source of a feed. It stores where the feed can be
 * found and how to import it.
 *
 * Information on how to import a feed is encapsulated in a FeedsImporter object
 * which is identified by the common id of the FeedsSource and the
 * FeedsImporter. More than one FeedsSource can use the same FeedsImporter
 * therefore a FeedsImporter never holds a pointer to a FeedsSource object, nor
 * does it hold any other information for a particular FeedsSource object.
 *
 * Classes extending FeedsPlugin can implement a sourceForm to expose
 * configuration for a FeedsSource object. This is for instance how FeedsFetcher
 * exposes a text field for a feed URL or how FeedsCSVParser exposes a select
 * field for choosing between colon or semicolon delimiters.
 *
 * It is important that a FeedsPlugin does not directly hold information about
 * a source but leave all storage up to FeedsSource. An instance of a
 * FeedsPlugin class only exists once per FeedsImporter configuration, while an
 * instance of a FeedsSource class exists once per feed_nid to be imported.
 *
 * As with FeedsImporter, the idea with FeedsSource is that it can be used
 * without actually saving the object to the database.
 */
class FeedsSource extends FeedsConfigurable {

  // Contains the node id of the feed this source info object is attached to.
  // Equals 0 if not attached to any node - i. e. if used on a
  // standalone import form within Feeds or by other API users.
  protected $feed_nid;

  // The FeedsImporter object that this source is expected to be used with.
  protected $importer;

  // A FeedsSourceState object holding the current import/clearing state of this
  // source.
  protected $state;

  // Fetcher result, used to cache fetcher result when batching.
  protected $fetcher_result;

  /**
   * Instantiate a unique object per class/id/feed_nid. Don't use
   * directly, use feeds_source() instead.
   */
  public static function instance($importer_id, $feed_nid = 0) {
    $class = variable_get('feeds_source_class', 'FeedsSource');
    static $instances = array();
    if (!isset($instances[$class][$importer_id][$feed_nid])) {
      $instances[$class][$importer_id][$feed_nid] = new $class($importer_id, $feed_nid);
    }
    return $instances[$class][$importer_id][$feed_nid];
  }

  /**
   * Constructor.
   */
  protected function __construct($importer_id, $feed_nid) {
    $this->feed_nid = $feed_nid;
    $this->importer = feeds_importer($importer_id);
    parent::__construct($importer_id);
    $this->load();
  }

  /**
   * Preview = fetch and parse a feed.
   *
   * @return
   *   FeedsParserResult object.
   *
   * @throws
   *   Throws Exception if an error occurs when fetching or parsing.
   */
  public function preview() {
    $result = $this->importer->fetcher->fetch($this);
    $result = $this->importer->parser->parse($this, $result);
    module_invoke_all('feeds_after_parse', $this, $result);
    return $result;
  }

  /**
   * Import a feed: execute fetching, parsing and processing stage.
   *
   * @return
   *   FEEDS_BATCH_COMPLETE if the import process finished. A decimal between
   *   0.0 and 0.9 periodic if import is still in progress.
   *
   * @throws
   *   Throws Exception if an error occurs when importing.
   */
  public function import() {
    try {
      // Fetch.
      if (empty($this->fetcher_result) || FEEDS_BATCH_COMPLETE == $this->progressParsing()) {
        $this->fetcher_result = $this->importer->fetcher->fetch($this);
        // Clean the parser's state, we are parsing an entirely new file.
        unset($this->state[get_class($this->importer->parser)]);
      }

      // Parse.
      $parser_result = $this->importer->parser->parse($this, $this->fetcher_result);
      module_invoke_all('feeds_after_parse', $this, $parser_result);

      // Process.
      $this->importer->processor->process($this, $parser_result);

      // Find out whether we are done.
      $result = $this->progressImporting();
      if ($result === FEEDS_BATCH_COMPLETE) {
        module_invoke_all('feeds_after_import', $this);
        unset($this->fetcher_result);
        unset($this->state);
      }
    }
    catch (Exception $e) {
      unset($this->fetcher_result);
      unset($this->state);
      $this->save();
      throw $e;
    }
    $this->save();
    return $result;
  }

  /**
   * Remove all items from a feed.
   *
   * @return
   *   FEEDS_BATCH_COMPLETE if the clearing process finished. A decimal between
   *   0.0 and 0.9 periodic if clearing is still in progress.
   *
   * @throws
   *   Throws Exception if an error occurs when clearing.
   */
  public function clear() {
    try {
      $this->importer->fetcher->clear($this);
      $this->importer->parser->clear($this);
      $this->importer->processor->clear($this);
      $result = $this->progressClearing();
      if ($result == FEEDS_BATCH_COMPLETE) {
        unset($this->state);
        module_invoke_all('feeds_after_clear', $this);
      }
    }
    catch (Exception $e) {
      unset($this->state);
      $this->save();
      throw $e;
    }
    $this->save();
    return $result;
  }

  /**
   * Report progress as float between 0 and 1. 1 = FEEDS_BATCH_COMPLETE.
   */
  public function progressParsing() {
    return $this->state($this->importer->parser)->progress();
  }

  /**
   * Report progress as float between 0 and 1. 1 = FEEDS_BATCH_COMPLETE.
   */
  public function progressImporting() {
    $fetcher = $this->state($this->importer->fetcher);
    $parser = $this->state($this->importer->parser);
    if ($fetcher->progress() == FEEDS_BATCH_COMPLETE && $parser->progress() == FEEDS_BATCH_COMPLETE) {
      return FEEDS_BATCH_COMPLETE;
    }
    // Fetching envelops parsing.
    $parser_progress = $parser->progress() / $fetcher->total;
    return $fetcher->progress - 1.0 + $parser_progress;
  }

  /**
   * Report progress on clearing.
   */
  public function progressClearing() {
    return $this->state($this->importer->processor)->progress();
  }

  /**
   * Return a state object for a given object. Lazy instantiates new states.
   *
   * @todo Rename getConfigFor() accordingly to config().
   *
   * @param FeedsSourceInterface $client
   *   An object that implements the FeedsSourceInterface, usually a fetcher,
   *   parser or processor plugin.
   *
   * @return
   *   The FeedsState object for the given client.
   */
  public function state(FeedsSourceInterface $client) {
    $class = get_class($client);
    if (!is_array($this->state)) {
      $this->state = array();
    }
    if (!isset($this->state[$class])) {
      $this->state[$class] = new FeedsState();
    }
    return $this->state[$class];
  }

  /**
   * Schedule this source.
   */
  public function schedule() {
    // Check whether any fetcher is overriding the import period.
    $period = $this->importer->config['import_period'];
    $fetcher_period = $this->importer->fetcher->importPeriod($this);
    if (is_numeric($fetcher_period)) {
      $period = $fetcher_period;
    }
    $job = array(
      'type' => $this->id,
      'id' => $this->feed_nid,
      // Schedule as soon as possible if a batch is active.
      'period' => $this->progressImporting() === FEEDS_BATCH_COMPLETE ? $period : 0,
      'periodic' => TRUE,
    );
    if ($job['period'] != FEEDS_SCHEDULE_NEVER) {
      JobScheduler::get('feeds_source_import')->set($job);
    }
    else {
      JobScheduler::get('feeds_source_import')->remove($job);
    }
  }

  /**
   * Save configuration.
   */
  public function save() {
    // Alert implementers of FeedsSourceInterface to the fact that we're saving.
    foreach ($this->importer->plugin_types as $type) {
      $this->importer->$type->sourceSave($this);
    }
    $config = $this->getConfig();

    // Store the source property of the fetcher in a separate column so that we
    // can do fast lookups on it.
    $source = '';
    if (isset($config[get_class($this->importer->fetcher)]['source'])) {
      $source = $config[get_class($this->importer->fetcher)]['source'];
    }
    $object = array(
      'id' => $this->id,
      'feed_nid' => $this->feed_nid,
      'config' => $config,
      'source' => $source,
      'state' => isset($this->state) ? $this->state : FALSE,
      'fetcher_result' => isset($this->fetcher_result) ? $this->fetcher_result : FALSE,
    );
    if (db_query_range("SELECT 1 FROM {feeds_source} WHERE id = :id AND feed_nid = :nid", 0, 1, array(':id' => $this->id, ':nid' => $this->feed_nid))->fetchField()) {
      drupal_write_record('feeds_source', $object, array('id', 'feed_nid'));
    }
    else {
      drupal_write_record('feeds_source', $object);
    }
  }

  /**
   * Load configuration and unpack.
   *
   * @todo Patch CTools to move constants from export.inc to ctools.module.
   */
  public function load() {
    if ($record = db_query("SELECT config, state, fetcher_result FROM {feeds_source} WHERE id = :id AND feed_nid = :nid", array(':id' => $this->id, ':nid' => $this->feed_nid))->fetch()) {
      // While FeedsSource cannot be exported, we still use CTool's export.inc
      // export definitions.
      ctools_include('export');
      $this->export_type = EXPORT_IN_DATABASE;
      $this->config = unserialize($record->config);
      if (!empty($record->state)) {
        $this->state = unserialize($record->state);
      }
      if (!empty($record->fetcher_result)) {
        $this->fetcher_result = unserialize($record->fetcher_result);
      }
    }
  }

  /**
   * Delete configuration. Removes configuration information
   * from database, does not delete configuration itself.
   */
  public function delete() {
    // Alert implementers of FeedsSourceInterface to the fact that we're
    // deleting.
    foreach ($this->importer->plugin_types as $type) {
      $this->importer->$type->sourceDelete($this);
    }
    db_delete('feeds_source')
      ->condition('id', $this->id)
      ->condition('feed_nid', $this->feed_nid)
      ->execute();
    // Remove from schedule.
    $job = array(
      'type' => $this->id,
      'id' => $this->feed_nid,
    );
    JobScheduler::get('feeds_source_import')->remove($job);
  }

  /**
   * Only return source if configuration is persistent and valid.
   *
   * @see FeedsConfigurable::existing().
   */
  public function existing() {
    // If there is no feed nid given, there must be no content type specified.
    // If there is a feed nid given, there must be a content type specified.
    // Ensure that importer is persistent (= defined in code or DB).
    // Ensure that source is persistent (= defined in DB).
    if ((empty($this->feed_nid) && empty($this->importer->config['content_type'])) ||
        (!empty($this->feed_nid) && !empty($this->importer->config['content_type']))) {
      $this->importer->existing();
      return parent::existing();
    }
  }

  /**
   * Returns the configuration for a specific client class.
   *
   * @param FeedsSourceInterface $client
   *   An object that is an implementer of FeedsSourceInterface.
   *
   * @return
   *   An array stored for $client.
   */
  public function getConfigFor(FeedsSourceInterface $client) {
    return $this->config[get_class($client)];
  }

  /**
   * Sets the configuration for a specific client class.
   *
   * @param FeedsSourceInterface $client
   *   An object that is an implementer of FeedsSourceInterface.
   * @param $config
   *   The configuration for $client.
   *
   * @return
   *   An array stored for $client.
   */
  public function setConfigFor(FeedsSourceInterface $client, $config) {
    $this->config[get_class($client)] = $config;
  }

  /**
   * Return defaults for feed configuration.
   */
  public function configDefaults() {
    // Collect information from plugins.
    $defaults = array();
    foreach ($this->importer->plugin_types as $type) {
      if ($this->importer->$type->hasSourceConfig()) {
        $defaults[get_class($this->importer->$type)] = $this->importer->$type->sourceDefaults();
      }
    }
    return $defaults;
  }

  /**
   * Override parent::configForm().
   */
  public function configForm(&$form_state) {
    // Collect information from plugins.
    $form = array();
    foreach ($this->importer->plugin_types as $type) {
      if ($this->importer->$type->hasSourceConfig()) {
        $class = get_class($this->importer->$type);
        $config = isset($this->config[$class]) ? $this->config[$class] : array();
        $form[$class] = $this->importer->$type->sourceForm($config);
        $form[$class]['#tree'] = TRUE;
      }
    }
    return $form;
  }

  /**
   * Override parent::configFormValidate().
   */
  public function configFormValidate(&$values) {
    foreach ($this->importer->plugin_types as $type) {
      $class = get_class($this->importer->$type);
      if (isset($values[$class]) && $this->importer->$type->hasSourceConfig()) {
        $this->importer->$type->sourceFormValidate($values[$class]);
      }
    }
  }
}
